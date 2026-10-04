/*
 * test_fwfilterusb.c - test harness for the fwfilterusb.sys Wine driver
 *
 * Mimics exactly what the Fanatec SDK does:
 *
 *     hDevice = CreateFileW(L"\\.\FWFilterUsb", GENERIC_READ|GENERIC_WRITE, 0,
 *                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
 *     DeviceIoControl(hDevice, 0x226028, NULL, 0, buffer, 0x404, &bytesReturned, NULL);
 *     DeviceIoControl(hDevice, 0x22602c, NULL, 0, NULL, 0, &bytesReturned, NULL);
 *
 * and then decodes the 0x404-byte reply (40-byte zero header + wide string
 * registry key path). Exits 0 on success, 1 on failure.
 *
 * Build (plain mingw, no wine headers needed):
 *     i686-w64-mingw32-gcc -o test_fwfilterusb.exe test_fwfilterusb.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define IOCTL_FW_FILTER_USB_SETTINGS_RELOAD   0x226028
#define IOCTL_FW_FILTER_USB_CONTROL_RELOAD    0x22602c
#define FW_REPLY_TOTAL                        0x404
#define FW_REPLY_HEADER                       40

/* \\.\FWFilterUsb  (built from pieces to keep the escaping obvious) */
static const WCHAR device_name[] = {'\\','\\','.','\\','F','W','F','i','l','t','e','r','U','s','b',0};

int main(void)
{
    HANDLE h;
    BYTE buffer[FW_REPLY_TOTAL];
    DWORD bytesReturned = 0;
    BOOL ok;

    memset( buffer, 0xAA, sizeof(buffer) );

    h = CreateFileW( device_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL );
    if (h == INVALID_HANDLE_VALUE)
    {
        printf( "FAIL: CreateFileW failed, err=%lu\n", GetLastError() );
        return 1;
    }
    printf( "OK:   CreateFileW(\\\\.\\FWFilterUsb) -> handle %p\n", h );

    ok = DeviceIoControl( h, IOCTL_FW_FILTER_USB_SETTINGS_RELOAD, NULL, 0,
                           buffer, sizeof(buffer), &bytesReturned, NULL );
    if (!ok)
    {
        printf( "FAIL: DeviceIoControl(0x226028) failed, err=%lu\n", GetLastError() );
        CloseHandle( h );
        return 1;
    }
    printf( "OK:   DeviceIoControl(0x226028) -> %lu bytes\n", bytesReturned );

    /* The reload call below takes no buffers and resets bytesReturned,
     * so keep the reply size for the checks at the end. */
    DWORD reply_bytes = bytesReturned;

    /* FSWinCmdWheelConfig::ControlDeviceRegistryReload: no input/output
     * buffer, the SDK only checks the success flag. */
    ok = DeviceIoControl( h, IOCTL_FW_FILTER_USB_CONTROL_RELOAD, NULL, 0,
                          NULL, 0, &bytesReturned, NULL );
    if (!ok)
    {
        printf( "FAIL: DeviceIoControl(0x22602c) failed, err=%lu\n", GetLastError() );
        CloseHandle( h );
        return 1;
    }
    printf( "OK:   DeviceIoControl(0x22602c) -> success, %lu bytes\n", bytesReturned );
    CloseHandle( h );

    if (reply_bytes != FW_REPLY_TOTAL)
    {
        printf( "FAIL: expected %d bytes, got %lu\n", FW_REPLY_TOTAL, reply_bytes );
        return 1;
    }

    /* Verify the 40-byte header is zeroed. */
    int header_ok = 1;
    for (int i = 0; i < FW_REPLY_HEADER; i++)
        if (buffer[i] != 0) { header_ok = 0; break; }
    printf( "%s:   40-byte header zeroed\n", header_ok ? "OK" : "FAIL" );

    /* Decode the wide string at offset 40. */
    const WCHAR *path = (const WCHAR *)(buffer + FW_REPLY_HEADER);
    printf( "     registry key path = \"%ls\"\n", path );

    if (*path == 0)
    {
        printf( "FAIL: registry key path is empty (no Fanatec device key found?)\n" );
        return 1;
    }

    if (!header_ok) return 1;
    printf( "PASS: fwfilterusb.sys responded correctly\n" );
    return 0;
}
