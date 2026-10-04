/*
 * load_driver.c - start the fwfilterusb driver service via the SCM.
 *
 * This is exactly how a driver gets loaded on Windows: open the service
 * control manager, open the service, StartService(). The wine "wine kernel"
 * then calls ZwLoadDriver -> LoadLibrary -> DriverEntry for us.
 *
 * Build: x86_64-w64-mingw32-gcc -o load_driver.exe load_driver.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char **argv)
{
    const WCHAR *name = L"fwfilterusb";
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;

    if (argc > 1)
    {
        int len = MultiByteToWideChar( CP_ACP, 0, argv[1], -1, NULL, 0 );
        WCHAR *tmp = (WCHAR *)malloc( len * sizeof(WCHAR) );
        MultiByteToWideChar( CP_ACP, 0, argv[1], -1, tmp, len );
        name = tmp;
    }

    scm = OpenSCManagerW( NULL, NULL, SC_MANAGER_ALL_ACCESS );
    if (!scm) { printf( "FAIL: OpenSCManager err=%lu\n", GetLastError() ); return 1; }

    svc = OpenServiceW( scm, name, SERVICE_ALL_ACCESS );
    if (!svc)
    {
        printf( "FAIL: OpenService(%s) err=%lu (is the service registered?)\n", name, GetLastError() );
        CloseServiceHandle( scm );
        return 1;
    }

    if (!StartServiceW( svc, 0, NULL ) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
    {
        printf( "FAIL: StartService(%s) err=%lu\n", name, GetLastError() );
        CloseServiceHandle( svc );
        CloseServiceHandle( scm );
        return 1;
    }

    if (QueryServiceStatus( svc, &ss ))
        printf( "OK:   service %s state=%lu (SERVICE_RUNNING=%d)\n", name, ss.dwCurrentState, SERVICE_RUNNING );
    else
        printf( "WARN: QueryServiceStatus err=%lu\n", GetLastError() );

    CloseServiceHandle( svc );
    CloseServiceHandle( scm );
    return 0;
}
