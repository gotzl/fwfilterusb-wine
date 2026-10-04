# fwfilterusb.sys

Emulation of Fanatec's `FWFilterUsb` kernel driver, so that FullForce FFB and the
wheel LEDs work within wine.

Unofficial project, use at your own risk - see [Disclaimer](#disclaimer).

---

## Install

From a release package (`fwfilterusb-<tag>.tar.gz` from GitHub Releases):

```sh
tar -xzf fwfilterusb-<tag>.tar.gz
cd fwfilterusb-<tag>
```

From source (needs a MinGW cross-compiler and Wine with `winegcc` + headers):

```sh
make all          # builds fwfilterusb-x64.sys (64-bit) and fwfilterusb.sys (32-bit)
# or: make x64  /  make i386
# override the toolchain if needed:
# make x64 WINEGCC=/path/to/winegcc WINEINC=/path/to/headers
```

Then run the installer (same in both cases).
Proton (requires `protontricks` to be installed):

```sh
./setup_ftec_fwfilter.sh --proton <APPID>   # APPID = Steam app id of your game
```

Standalone Wine:

```sh
export WINEPREFIX=...
./setup_ftec_fwfilter.sh
```

`--install` (the default) performs a full install. It
* detects your wheel's PID via `lsusb`,
* imports registry keys,
* copies the `.sys` into `C:\windows\system32\drivers\`,
* registers the service and sets the `DeviceKey` override.

If several wheels are connected, pass `--pid` explicitly.
For more options see `./setup_ftec_fwfilter.sh --help`.

The driver is active on the next `wineboot` (i.e. next time the game is started).

### RGB configuration (`RimType`)

Different wheel-rim LED configurations need different `RimType` registry values.
`--rim` alone switches `RimType` on an already-installed prefix (no
reinstall, no driver copy):

```sh
# 9 RGB LEDs (default, `RimType 0x15`):
./setup_ftec_fwfilter.sh --proton <APPID> --rim 9rgb
# Single RGB LED (`RimType 0x0F`):
./setup_ftec_fwfilter.sh --proton <APPID> --rim single-rgb
# 9 non-RGB LEDs (`RimType 0x14`):
./setup_ftec_fwfilter.sh --proton <APPID> --rim 9mono
```


---

## Verifying

```sh
make test
# expected tail:
#   OK:   DeviceIoControl(0x226028) -> 1028 bytes
#   OK:   DeviceIoControl(0x22602c) -> success, 0 bytes
#   OK:   40-byte header zeroed
#          registry key path = "SYSTEM\CurrentControlSet\Enum\USB#VID_0EB7&..."
#   PASS: fwfilterusb.sys responded correctly
```

`make test` builds everything, installs into a throwaway wine prefix and runs
the SDK-mimicking client.

In-game, watch for `fwfilterusb` and `reg` wine-debug output (`WINEDEBUG=+fwfilterusb,+reg,...`).

---

## Background

Titles that use the Fanatec SDK do not
talk to the wheel through the normal HID interface for configuration. They open
a device created by Fanatec's **Windows kernel filter driver** `fwfilterusb.sys`
and ask it where the wheel's configuration lives in the registry:

```c
hDevice = CreateFileW(L"\\.\FWFilterUsb", GENERIC_READ|GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
DeviceIoControl(hDevice, 0x226028 /* ...REGISTRY_SETTINGS_RELOAD */,
                NULL, 0, buffer, 0x404, &bytesReturned, NULL);
```

The 0x404-byte (1028-byte) reply is a zeroed 40-byte header followed by a wide
string containing the registry hardware key path, e.g.

```
SYSTEM\CurrentControlSet\Enum\USB#VID_0EB7&PID_0020#001#6&12345678&0&000
```

The game then reads `HKLM\<that path>` for `SystemReport` / `SystemConfig` /
`RimType` / etc. - FullForce FFB and the wheel LEDs both run off these keys
(the SDK reads `RimType` for the LEDs).

`fwfilterusb.c` is a native PE (subsystem `IMAGE_SUBSYSTEM_NATIVE`) loaded by
Wine's "wine kernel" (`winedevice.exe`) via `ZwLoadDriver`, exactly like
`winehid.sys` or `winebus.sys`. On `DriverEntry` it creates the device
`\Device\FWFilterUsb` and the symbolic link `\??\FWFilterUsb` (which is what
`\\.\FWFilterUsb` resolves to). Its `IRP_MJ_DEVICE_CONTROL` handler answers the
`0x226028` IOCTL with the 1028-byte reply above, and `0x22602c` (registry
reload, called with no buffers) with plain success - the SDK only checks the
status flag.

The registry key path comes from the `DeviceKey` override
(`HKLM\SYSTEM\CurrentControlSet\Services\fwfilterusb\DeviceKey`, a `REG_SZ`
holding the full relative key path) that `setup_ftec_fwfilter.sh` writes.
Without it the driver reports "no device".

### Loading

The driver is registered as a service with `Group=WinePlugPlay` and
`Start=2` (automatic). The winedevice loads it on `wineboot`, so **no explicit
`StartService` is required** - the device is available as soon as the prefix is
up.

> **Architecture:** in a WoW64 prefix the 32-bit and 64-bit games use different
> `winedevice.exe` instances, so the `.sys` must match the game's architecture.
> Most affected titles are **x86-64**, so the
> 64-bit driver (`fwfilterusb-x64.sys`) is usually the one that matters.
>
> A 32-bit build (`fwfilterusb.sys`) is also provided. **Caveat:** in the
> current WoW64 wine build the service control manager runs inside the 64-bit
> `winedevice.exe`, so a 32-bit `.sys` cannot be loaded (the 64-bit loader
> rejects it with `STATUS_DLL_INIT_FAILED`), and the 32-bit winedevice never
> gets the driver. The 64-bit path is fully working; the 32-bit build is
> included for completeness / future wine builds where the SCM is per-arch.

The `SystemReport` blob must have **byte 0 = 0x02** (FullForce enabled) - the
default in the registry template is correct.

---

## Disclaimer

This software is provided as-is, without warranty of any kind. Use at your
own risk - it installs a driver shim and writes to your Wine/Proton registry.

This is an independent, unofficial project with no affiliation with, or
endorsement by, Fanatec. Fanatec and FullForce are trademarks of their
respective owners.
