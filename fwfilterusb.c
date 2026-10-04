/*
 * fwfilterusb.sys - Wine emulation of the Fanatec FWFilterUsb filter driver.
 *
 * Answers the Fanatec SDK's DeviceIoControl(0x226028) with the registry
 * hardware key path (40-byte zeroed header + wide string, 1028 bytes total)
 * and 0x22602c (registry reload, no buffers) with plain success.
 *
 * The key is the DeviceKey override in our own service key, written by the
 * installer. Without it no key is reported.
 *
 * See README.md for a more detailed protocol description.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. See COPYING for the full text.
 */

#include "wine/debug.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winioctl.h"
#include "winternl.h"
#include "ntstatus.h"
#include "ddk/wdm.h"

WINE_DEFAULT_DEBUG_CHANNEL( fwfilterusb );

/* The proprietary "reload registry settings" IOCTL used by the Fanatec SDK. */
#define IOCTL_FW_FILTER_USB_SETTINGS_RELOAD   0x226028
/* FSWinCmdWheelConfig::ControlDeviceRegistryReload: no input/output buffer,
 * just asks the driver to re-read the registry settings. */
#define IOCTL_FW_FILTER_USB_CONTROL_RELOAD    0x22602c

/* Reply layout (see header comment). */
#define FW_REPLY_TOTAL                        0x404   /* 1028 bytes            */
#define FW_REPLY_HEADER                       40      /* zeroed header bytes   */
#define FW_REPLY_STRING_OFF                   40      /* wide string offset    */

/* Registry locations. */
#define FW_SERVICE_KEY                        L"SYSTEM\\CurrentControlSet\\Services\\fwfilterusb"
#define FW_DEVICE_KEY_VALUE                   L"DeviceKey"

static PDEVICE_OBJECT device_object;

/*
 * Fill `dst` (capacity `dst_len` wide chars) with the registry hardware key
 * path the game should read, as a null-terminated wide string of the form
 * "SYSTEM\CurrentControlSet\Enum\USB#VID_0EB7&PID_XXXX#<instance>".
 *
 * Returns TRUE on success, FALSE if no suitable key could be found.
 */
static BOOLEAN resolve_registry_key( WCHAR *dst, size_t dst_len )
{
    HKEY key;

    /* Explicit override in the driver's own service key (written by the
     * installer). Without it there is nothing to report. */
    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, FW_SERVICE_KEY, 0, KEY_READ, &key ))
    {
        WCHAR value[512];
        DWORD size = sizeof(value), type = 0;

        if (!RegQueryValueExW( key, FW_DEVICE_KEY_VALUE, NULL, &type, (LPBYTE)value, &size ) &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            /* value is already a full relative path (no HKEY_LOCAL_MACHINE\ prefix) */
            size_t len = lstrlenW( value );
            RegCloseKey( key );
            if (len < dst_len)
            {
                lstrcpynW( dst, value, dst_len );
                TRACE( "using override key %s\n", debugstr_w( dst ) );
                return TRUE;
            }
            ERR( "override key too long (%lu >= %lu)\n", (unsigned long)len, (unsigned long)dst_len );
        }
        else
            RegCloseKey( key );
    }

    TRACE( "no DeviceKey override found\n" );
    return FALSE;
}

/*
 * Build the 0x404-byte reply in `buffer` (at least `outsize` bytes).
 *
 * Returns the number of bytes written, or 0 if the reply could not be built.
 */
static ULONG build_reply( PVOID buffer, ULONG outsize )
{
    WCHAR *path = (WCHAR *)((CHAR *)buffer + FW_REPLY_STRING_OFF);
    size_t string_capacity;
    BOOLEAN found;

    if (outsize < FW_REPLY_TOTAL)
    {
        TRACE( "output buffer too small: %lu < %#lx\n", (unsigned long)outsize, FW_REPLY_TOTAL );
        return 0;
    }

    /* Zero the whole reply (40-byte header + the string area). */
    RtlZeroMemory( buffer, FW_REPLY_TOTAL );

    string_capacity = (FW_REPLY_TOTAL - FW_REPLY_STRING_OFF) / sizeof(WCHAR);
    found = resolve_registry_key( path, string_capacity );
    if (!found)
    {
        /* Leave a zeroed reply; the game will treat this as "no device". */
        TRACE( "no registry key to report\n" );
        return 0;
    }

    return FW_REPLY_TOTAL;
}

static NTSTATUS WINAPI fw_create( PDEVICE_OBJECT device, PIRP irp )
{
    TRACE( "device=%p irp=%p\n", device, irp );
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI fw_close( PDEVICE_OBJECT device, PIRP irp )
{
    TRACE( "device=%p irp=%p\n", device, irp );
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI fw_device_control( PDEVICE_OBJECT device, PIRP irp )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation( irp );
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "device=%p irp=%p code=%#lx outsize=%lu\n", device, irp, code, (unsigned long)outsize );

    switch (code)
    {
    case IOCTL_FW_FILTER_USB_SETTINGS_RELOAD:
    {
        ULONG written;

        if (!irp->AssociatedIrp.SystemBuffer)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        if ((code & 3) != METHOD_BUFFERED)
        {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        written = build_reply( irp->AssociatedIrp.SystemBuffer, outsize );
        if (!written)
        {
            /* No device configured: report success with zero bytes written so
             * the SDK cleanly reports "no FullForce device" instead of crashing. */
            irp->IoStatus.Information = 0;
            break;
        }

        irp->IoStatus.Information = written;
        break;
    }
    case IOCTL_FW_FILTER_USB_CONTROL_RELOAD:
    {
        /* No input or output buffer (the SDK passes NULL/0 for both). Emulate
         * the real driver's "reload" by re-resolving the registry key, which
         * also picks up any DeviceKey override changes made at runtime. The
         * SDK only checks the success flag, so report STATUS_SUCCESS even
         * when no device key is found -- the driver *is* installed. */
        WCHAR path[512];

        if (resolve_registry_key( path, ARRAY_SIZE( path ) ))
            TRACE( "registry settings reloaded: %s\n", debugstr_w( path ) );
        else
            TRACE( "registry settings reload: no device key found\n" );
        irp->IoStatus.Information = 0;
        break;
    }
    default:
        TRACE( "unsupported IOCTL %#lx\n", code );
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static void WINAPI fw_unload( PDRIVER_OBJECT driver )
{
    TRACE( "(%p)\n", driver );
    if (device_object)
    {
        UNICODE_STRING link = RTL_CONSTANT_STRING( L"\\\\??\\FWFilterUsb" );
        IoDeleteSymbolicLink( &link );
        IoDeleteDevice( device_object );
        device_object = NULL;
    }
}

NTSTATUS WINAPI DriverEntry( PDRIVER_OBJECT driver, PUNICODE_STRING registry_path )
{
    UNICODE_STRING device_name = RTL_CONSTANT_STRING( L"\\Device\\FWFilterUsb" );
    UNICODE_STRING link_name   = RTL_CONSTANT_STRING( L"\\??\\FWFilterUsb" );
    NTSTATUS status;

    TRACE( "(%p, %s)\n", driver, debugstr_w( registry_path->Buffer ) );

    driver->DriverUnload = fw_unload;
    driver->MajorFunction[IRP_MJ_CREATE] = fw_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = fw_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = fw_device_control;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = fw_device_control;

    status = IoCreateDevice( driver, 0, &device_name, FILE_DEVICE_UNKNOWN, 0, FALSE, &device_object );
    if (NT_SUCCESS( status ))
    {
        status = IoCreateSymbolicLink( &link_name, &device_name );
        if (NT_SUCCESS( status ))
            TRACE( "created \\\\.\\FWFilterUsb\n" );
        else
            ERR( "IoCreateSymbolicLink failed: %#lx\n", status );
    }
    else
        ERR( "IoCreateDevice failed: %#lx\n", status );

    return STATUS_SUCCESS;
}
