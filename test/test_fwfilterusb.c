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
 * registry key path).
 *
 * The second part exercises the sysfs sync against a fake tree under
 * C:\fwtest. Exits 0 on success, 1 on failure.
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
#define ARRAY_SIZE(x)                         (sizeof(x) / sizeof((x)[0]))

#define HW_KEY_ROOT   L"SYSTEM\\CurrentControlSet\\Services\\fwfilterusb"
#define VAL_DEVICEKEY L"DeviceKey"
#define VAL_RIMTYPE   L"RimType"
#define VAL_WHEELTYPE L"WheelType"
#define VAL_SYSPATH   L"SysfsPath"

#define FWTEST_ROOT   L"C:\\fwtest"
#define FWTEST_TREE   FWTEST_ROOT L"\\module\\hid_fanatec\\drivers\\hid:fanatec"
#define RIM_FILE      L"\\rim_id"
#define WHEEL_FILE    L"\\wheel_type"

/* Template fallback RimType (SDK value) in ftec_fwfilter.reg. raw rim_id
 * 0x18 -> 0x15 through the driver's rim_id_to_sdk() mapping. */
#define STATIC_RIM    0x15

static int failures = 0;

static LONG read_dword( const WCHAR *devkey, const WCHAR *name )
{
    HKEY key;
    DWORD value = 0, size = sizeof(value), type = 0;
    LONG r;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, devkey, 0, KEY_QUERY_VALUE, &key )) return -1;
    r = RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size );
    RegCloseKey( key );
    if (r || type != REG_DWORD) return -1;
    return (LONG)value;
}

static void set_service_sz( const WCHAR *name, const WCHAR *data )
{
    HKEY svc;
    LONG r;
    if ((r = RegOpenKeyExW( HKEY_LOCAL_MACHINE, HW_KEY_ROOT, 0, KEY_SET_VALUE, &svc )))
    {
        printf( "FAIL: open %ls: err %lu\n", HW_KEY_ROOT, r );
        failures++;
        return;
    }
    r = RegSetValueExW( svc, name, 0, REG_SZ, (const BYTE *)data,
                        (lstrlenW( data ) + 1) * sizeof(WCHAR) );
    RegCloseKey( svc );
    if (r) { printf( "FAIL: set %ls: err %lu\n", name, r ); failures++; }
}

static void del_service_value( const WCHAR *name )
{
    HKEY svc;
    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, HW_KEY_ROOT, 0, KEY_SET_VALUE, &svc ))
    {
        RegDeleteValueW( svc, name );
        RegCloseKey( svc );
    }
}

static int read_devicekey_path( WCHAR *path, size_t path_len )
{
    HKEY svc;
    DWORD size = (DWORD)(path_len * sizeof(WCHAR)), type = 0;
    LONG r;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, HW_KEY_ROOT, 0, KEY_READ, &svc )) return 0;
    r = RegQueryValueExW( svc, VAL_DEVICEKEY, NULL, &type, (BYTE *)path, &size );
    RegCloseKey( svc );
    if (r || (type != REG_SZ && type != REG_EXPAND_SZ) || !path[0]) return 0;
    return 1;
}

static void check_value( const char *what, const WCHAR *devkey, const WCHAR *name, LONG expected )
{
    LONG got = read_dword( devkey, name );
    if (got != expected)
    {
        printf( "FAIL: %s: %ls = %ld, expected %#lx\n", what, name, got, (unsigned long)expected );
        failures++;
    }
    else
        printf( "OK:   %s (%ls %#lx)\n", what, name, (unsigned long)got );
}

static void check_rim( const char *what, const WCHAR *devkey, LONG expected )
{
    check_value( what, devkey, VAL_RIMTYPE, expected );
}

static void do_reload( HANDLE h )
{
    DWORD bytes = 0;
    if (!DeviceIoControl( h, IOCTL_FW_FILTER_USB_CONTROL_RELOAD, NULL, 0, NULL, 0, &bytes, NULL ))
    {
        printf( "FAIL: DeviceIoControl(0x22602c) failed, err=%lu\n", GetLastError() );
        failures++;
    }
}

/* mkdir -p. */
static void mkdir_ok( const WCHAR *path )
{
    WCHAR tmp[512];
    WCHAR *p;

    lstrcpynW( tmp, path, ARRAY_SIZE(tmp) );
    for (p = tmp + 3; *p; p++)  /* skip "C:\" */
    {
        if (*p == '\\')
        {
            WCHAR saved = *p;
            *p = 0;
            CreateDirectoryW( tmp, NULL );  /* ERROR_ALREADY_EXISTS is fine */
            *p = saved;
        }
    }
    CreateDirectoryW( tmp, NULL );
}

static void write_ascii_file( const WCHAR *path, const char *content )
{
    HANDLE h = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    DWORD written = 0;
    if (h == INVALID_HANDLE_VALUE)
    {
        printf( "FAIL: cannot create %ls (err %lu)\n", path, GetLastError() );
        failures++;
        return;
    }
    WriteFile( h, content, (DWORD)strlen( content ), &written, NULL );
    CloseHandle( h );
}

/* <tree>\0003:0EB7:<pid><suffix>, e.g. ...\0003:0EB7:0020.0013 */
static void instance_dir( WCHAR *dst, size_t dst_len, const WCHAR *tree,
                          const WCHAR *pid, const WCHAR *suffix )
{
    lstrcpynW( dst, tree, (int)dst_len );
    lstrcatW( dst, L"\\0003:0EB7:" );
    lstrcatW( dst, pid );
    lstrcatW( dst, suffix );
}

static void run_sysfs_tests( HANDLE h, const char *pid )
{
    WCHAR devkey[512], dir_a[570], dir_b[570], dir_c[570], junk[570];
    WCHAR rim_a[600], wheel_a[600];
    WCHAR glob[620], glob2[620], glob3[620], glob4[620];
    WCHAR wpid[8];
    LONG rim;

    MultiByteToWideChar( CP_ACP, 0, pid, -1, wpid, 8 );

    if (!read_devicekey_path( devkey, ARRAY_SIZE(devkey) ))
    {
        printf( "SKIP: no DeviceKey service value; sysfs sync part not run\n" );
        return;
    }
    printf( "     device key = \"%ls\"\n", devkey );

    /* dir_a has the sync attributes; dir_b is a sibling HID interface
     * without them; dir_c is an unrelated PID. */
    instance_dir( dir_a, ARRAY_SIZE(dir_a), FWTEST_TREE, wpid, L".0013" );
    instance_dir( dir_b, ARRAY_SIZE(dir_b), FWTEST_TREE, wpid, L".001D" );
    instance_dir( dir_c, ARRAY_SIZE(dir_c), FWTEST_TREE, L"6204", L".001C" );
    mkdir_ok( FWTEST_ROOT );
    mkdir_ok( FWTEST_TREE );
    mkdir_ok( dir_a );
    mkdir_ok( dir_b );
    mkdir_ok( dir_c );

    lstrcpynW( rim_a, dir_a, ARRAY_SIZE(rim_a) );
    lstrcatW( rim_a, RIM_FILE );
    lstrcpynW( wheel_a, dir_a, ARRAY_SIZE(wheel_a) );
    lstrcatW( wheel_a, WHEEL_FILE );

    /* glob = the installed SysfsPath; glob2 doubles the '*' (invalid),
     * glob4/glob3 match directories without the attribute. */
    lstrcpynW( glob, FWTEST_TREE, ARRAY_SIZE(glob) );
    lstrcatW( glob, L"\\0003:0EB7:" );
    lstrcatW( glob, wpid );
    lstrcatW( glob, L".*" );
    lstrcpynW( glob2, glob, ARRAY_SIZE(glob2) );
    lstrcatW( glob2, L"|bogus*" );               /* now two '*' -> invalid */
    lstrcpynW( junk, FWTEST_TREE, ARRAY_SIZE(junk) );
    lstrcatW( junk, L"\\nogroup" );              /* no sync attributes */
    mkdir_ok( junk );
    lstrcpynW( glob3, junk, ARRAY_SIZE(glob3) ); /* literal dir, no attr */
    lstrcpynW( glob4, FWTEST_TREE, ARRAY_SIZE(glob4) );
    lstrcatW( glob4, L"\\nog*" );                /* matches junk dir only */

    set_service_sz( VAL_SYSPATH, glob );
    printf( "     SysfsPath = \"%ls\"\n", glob );

    rim = read_dword( devkey, VAL_RIMTYPE );
    printf( "     static template RimType = %#lx\n", (unsigned long)rim );

    /* every table attribute must parse, so wheel_type is written alongside
     * rim_id for the whole run (dir_a carries the full set). Values written
     * to rim_id are raw wheel bytes; the registry RimType is the mapped SDK
     * enum (rim_id -> RimType). */
    write_ascii_file( wheel_a, "0x11\n" );

    /* raw 0x0F -> RimType 20 (0x14); dir_b and dir_c must be skipped */
    write_ascii_file( rim_a, "0x0F\n" );
    do_reload( h );
    check_rim( "hex rim_id 0x0F -> RimType 0x14", devkey, 0x14 );
    check_value( "wheel_type synced", devkey, VAL_WHEELTYPE, 0x11 );

    /* decimal raw 21 (0x15) -> RimType 17 (0x11) */
    write_ascii_file( rim_a, "21\n" );
    do_reload( h );
    check_rim( "decimal rim_id 21 -> RimType 0x11", devkey, 0x11 );

    /* malformed content: no change */
    write_ascii_file( rim_a, "x!\n" );
    do_reload( h );
    check_rim( "malformed ignored", devkey, 0x11 );

    /* attribute gone: no change */
    DeleteFileW( rim_a );
    do_reload( h );
    check_rim( "missing file ignored", devkey, 0x11 );

    /* raw 0x14 -> RimType 16 (0x10) */
    write_ascii_file( rim_a, "0x14\n" );
    do_reload( h );
    check_rim( "hex rim_id 0x14 -> RimType 0x10", devkey, 0x10 );

    /* attribute-less dir (glob) and plain literal dir: no change in either */
    set_service_sz( VAL_SYSPATH, glob4 );
    do_reload( h );
    check_rim( "glob without attribute dirs ignored", devkey, 0x10 );
    set_service_sz( VAL_SYSPATH, glob3 );
    do_reload( h );
    check_rim( "literal dir without rim_id ignored", devkey, 0x10 );

    /* invalid glob (two '*'): no change, IOCTL still succeeds */
    set_service_sz( VAL_SYSPATH, glob2 );
    do_reload( h );
    check_rim( "invalid glob ignored", devkey, 0x10 );

    /* cleanup: drop the sync values and restore the template RimType */
    {
        HKEY key;
        DWORD value = STATIC_RIM;
        write_ascii_file( rim_a, "0x18\n" );  /* raw 0x18 -> RimType 0x15 */
        if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, devkey, 0, KEY_SET_VALUE, &key ))
        {
            RegSetValueExW( key, VAL_RIMTYPE, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
            RegCloseKey( key );
        }
        del_service_value( VAL_SYSPATH );
    }

    if (!failures)
        printf( "PASS: RimType sysfs sync works\n" );
    else
        printf( "FAIL: RimType sysfs sync: %d failure(s)\n", failures );
}

/* -------------------------------------------------------------------------- */

int main( int argc, char **argv )
{
    HANDLE h;
    BYTE buffer[FW_REPLY_TOTAL];
    DWORD bytesReturned = 0;
    BOOL ok;
    const char *pid = (argc > 1) ? argv[1] : "0020";

    memset( buffer, 0xAA, sizeof(buffer) );

    h = CreateFileW( L"\\\\.\\FWFilterUsb", GENERIC_READ | GENERIC_WRITE, 0, NULL,
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

    /* Reopen the device for the sysfs-sync part. */
    h = CreateFileW( L"\\\\.\\FWFilterUsb", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL );
    if (h == INVALID_HANDLE_VALUE)
    {
        printf( "FAIL: reopen CreateFileW failed, err=%lu\n", GetLastError() );
        return 1;
    }
    run_sysfs_tests( h, pid );
    CloseHandle( h );

    return failures ? 1 : 0;
}
