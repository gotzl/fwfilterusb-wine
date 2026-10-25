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
 * At startup and on the SDK's reload IOCTLs, the driver mirrors RimType and
 * WheelType from the sysfs directory named by the SysfsPath service value.
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
#include "winnls.h"
#include <stdlib.h>
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
/* DOS glob (at most one '*') to the sysfs device dir; see sync_from_sysfs. */
#define FW_SYSFS_PATH_VALUE                   L"SysfsPath"

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

    /* Installer-provided DeviceKey override; nothing to report without it. */
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

/* --- sysfs sync --------------------------------------------------------------- */

#define FW_MAX_DIR_ENTRIES   128

/* Map sysfs rim_id to the SDK's RimType (mirrors Fanatec's
 * FWFUUtilDeviceSteeringWheelTypeGet); unknown ids use the generic rim. */
static DWORD rim_id_to_sdk( DWORD raw )
{
    switch (raw)
    {
    case 1:  return 1;   /* CSWRBMW */
    case 2:  return 2;   /* CSWRFORM */
    case 3:  return 3;   /* CSWRPORSCHE */
    case 4:  return 4;   /* CSWRUH */
    case 6:  return 5;   /* CSWRUHX */
    case 7:  return 6;   /* CSLRP1X */
    case 8:  return 7;   /* CSLRP1PS4 */
    case 9:  return 8;   /* CSLRMCLV1_0 */
    case 10: return 9;   /* CSWRFORMV2 */
    case 11: return 10;  /* CSLRMCLV1_1 */
    case 12: return 11;  /* PHUB */
    case 0x0d: case 0x12: return 14; /* CSLRWRC */
    case 0x0e: return 19; /* PSWBENT */
    case 0x0f: case 0x17: return 20; /* PSWBMW */
    case 0x10: return 12; /* DDRGT */
    case 0x11: return 13; /* CSLRUH */
    case 0x13: return 15; /* CSSWBMWV2 */
    case 0x14: return 16; /* CSSWRS */
    case 0x15: return 17; /* CSUHV2 */
    case 0x16: return 18; /* CSSWF1V2 */
    case 0x18: return 21; /* DDRGTX */
    case 0xfe: return 22; /* GENERIC */
    case 0xff: return 23; /* EXT_INFO */
    default:   return 22; /* GENERIC: the SDK's rim for an unmapped id */
    }
}

/* sysfs attribute -> registry value, with optional transform; a directory is
 * used only when every attribute in this table parses. */
static const struct
{
    const WCHAR *attr;
    const WCHAR *value_name;
    DWORD (*xform)( DWORD );
}
fw_sysfs_sync[] =
{
    { L"rim_id", L"RimType", rim_id_to_sdk },
    { L"wheel_type", L"WheelType", NULL },
};

#define FW_SYSFS_SYNC_COUNT   (ARRAY_SIZE( fw_sysfs_sync ))

/* Read the SysfsPath service value into `pattern`; FALSE when unset. */
static BOOLEAN read_sysfs_path( WCHAR *pattern, size_t pattern_len )
{
    HKEY svc;
    DWORD size, type;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, FW_SERVICE_KEY, 0, KEY_READ, &svc )) return FALSE;
    size = (DWORD)(pattern_len * sizeof(WCHAR));
    if (RegQueryValueExW( svc, FW_SYSFS_PATH_VALUE, NULL, &type, (BYTE *)pattern, &size ) ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !pattern[0])
    {
        RegCloseKey( svc );
        TRACE( "no %s value configured; nothing to sync\n", debugstr_w(FW_SYSFS_PATH_VALUE) );
        return FALSE;
    }
    RegCloseKey( svc );
    return TRUE;
}

/* Read a sysfs integer (decimal or 0x-hex, trailing newline ok). */
static BOOLEAN read_sysfs_dword( const WCHAR *file, DWORD *out )
{
    char buf[32], *end;
    unsigned long value;
    DWORD got;
    HANDLE h;

    h = CreateFileW( file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!ReadFile( h, buf, sizeof(buf) - 1, &got, NULL ))
    {
        CloseHandle( h );
        return FALSE;
    }
    CloseHandle( h );
    if (!got) return FALSE;
    buf[got] = 0;

    value = strtoul( buf, &end, 0 );
    if (end == buf) return FALSE;
    *out = (DWORD)value;
    return TRUE;
}

/* Write all mapped values onto key; TRUE only if every attribute parsed. */
static BOOLEAN sync_dir( const WCHAR *dir_dos, HKEY key )
{
    DWORD values[FW_SYSFS_SYNC_COUNT], old, size, type;
    WCHAR item[1080];
    size_t k;
    BOOLEAN had_old;

    for (k = 0; k < FW_SYSFS_SYNC_COUNT; k++)
    {
        lstrcpynW( item, dir_dos, ARRAY_SIZE( item ) );
        lstrcatW( item, L"\\" );
        lstrcatW( item, fw_sysfs_sync[k].attr );
        if (!read_sysfs_dword( item, &values[k] )) return FALSE;
        if (fw_sysfs_sync[k].xform) values[k] = fw_sysfs_sync[k].xform( values[k] );
    }

    for (k = 0; k < FW_SYSFS_SYNC_COUNT; k++)
    {
        old = 0;
        size = sizeof(old);
        type = 0;
        had_old = !RegQueryValueExW( key, fw_sysfs_sync[k].value_name, NULL, &type,
                                     (BYTE *)&old, &size ) && type == REG_DWORD;
        if (had_old && old == values[k]) continue;
        type = REG_DWORD;
        if (!RegSetValueExW( key, fw_sysfs_sync[k].value_name, 0, type,
                             (BYTE *)&values[k], sizeof(values[k]) ))
            TRACE( "%s %s -> %lu\n", debugstr_w( fw_sysfs_sync[k].value_name ),
                   had_old ? "changed" : "was unset", (unsigned long)values[k] );
        else
            ERR( "%s write failed\n", debugstr_w( fw_sysfs_sync[k].value_name ) );
    }
    return TRUE;
}

/*
 * Resolve SysfsPath (a literal dir or a single-'*' glob) and mirror its
 * attributes onto the hardware key. The glob is matched by hand because
 * FindFirstFileW treats the ':' in "0003:0EB7" as an NTFS stream separator.
 */
static void sync_from_sysfs( void )
{
    WCHAR pattern[512], parent[512], search[516], cand[1040], hw_path[512];
    HANDLE find;
    WIN32_FIND_DATAW fd;
    DWORD entries = 0;
    int len, i, sep, lit_len, stars = 0;
    BOOLEAN synced = FALSE;
    HKEY key;

    if (!read_sysfs_path( pattern, ARRAY_SIZE( pattern ) )) return;
    TRACE( "sysfs path: %s\n", debugstr_w( pattern ) );

    len = lstrlenW( pattern );
    while (len > 1 && pattern[len - 1] == '\\') len--;
    pattern[len] = 0;

    for (i = 0; i < len; i++)
        if (pattern[i] == '*') stars++;
    if (stars > 1 || (stars && pattern[len - 1] != '*')) goto invalid;

    if (!resolve_registry_key( hw_path, ARRAY_SIZE( hw_path ) )) return;
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, hw_path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key ))
    {
        ERR( "cannot open hardware key %s for sysfs sync\n", debugstr_w( hw_path ) );
        return;
    }

    if (!stars)
    {
        synced = sync_dir( pattern, key );
    }
    else
    {
        sep = len - 2;                                   /* char before the '*' */
        while (sep > 0 && pattern[sep] != '\\') sep--;
        if (sep < 1) goto invalid;                       /* no base component */
        lit_len = len - sep - 2;                         /* leaf chars before the '*' */
        lstrcpynW( parent, pattern, sep + 1 );
        lstrcpynW( search, parent, ARRAY_SIZE( search ) );
        lstrcatW( search, L"\\*" );

        find = FindFirstFileW( search, &fd );
        if (find == INVALID_HANDLE_VALUE)
        {
            TRACE( "no directory entries under %s: err %lu\n",
                   debugstr_w( parent ), GetLastError() );
            RegCloseKey( key );
            return;
        }

        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!lstrcmpW( fd.cFileName, L"." ) || !lstrcmpW( fd.cFileName, L".." )) continue;
            if ((int)lstrlenW( fd.cFileName ) < lit_len) continue;
            if (lit_len &&
                CompareStringW( LOCALE_INVARIANT, NORM_IGNORECASE,
                                fd.cFileName, lit_len, pattern + sep + 1, lit_len ) != CSTR_EQUAL)
                continue;
            if (++entries > FW_MAX_DIR_ENTRIES) break;

            lstrcpynW( cand, parent, ARRAY_SIZE( cand ) );
            lstrcatW( cand, L"\\" );
            lstrcatW( cand, fd.cFileName );
            if (sync_dir( cand, key ))
            {
                TRACE( "synced from %s\n", debugstr_w( cand ) );
                synced = TRUE;
                break;
            }
        } while (FindNextFileW( find, &fd ));

        FindClose( find );
    }

    RegCloseKey( key );
    if (!synced)
        TRACE( "no directory with a full sync attribute set\n" );
    return;

invalid:
    ERR( "invalid %s value: %s\n", debugstr_w( FW_SYSFS_PATH_VALUE ), debugstr_w( pattern ) );
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

        sync_from_sysfs();

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
         * the real driver's "reload" by re-resolving the key and syncing from
         * sysfs (which also picks up DeviceKey changes made at runtime). The
         * SDK only checks the success flag, so report STATUS_SUCCESS even when
         * no device key is found -- the driver *is* installed. */
        WCHAR path[512];

        sync_from_sysfs();

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

    /* Initial sync; later updates only on the SDK's reload IOCTLs. */
    sync_from_sysfs();

    return STATUS_SUCCESS;
}
