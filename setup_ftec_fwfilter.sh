#!/bin/bash
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup_ftec_fwfilter.sh -- install/uninstall the fwfilterusb.sys Wine driver
# (Fanatec FullForce FFB + LEDs) and its registry keys.
#
# Titles using the Fanatec SDK open
# \\.\FWFilterUsb and issue DeviceIoControl(0x226028) to learn where the
# wheel's configuration lives in the registry. That device is created by
# Fanatec's Windows kernel filter driver, which does not exist in Wine. This
# tool installs a small Wine driver (fwfilterusb.sys) that emulates it.
#
# Unofficial project, not affiliated with Fanatec. Use at your own risk.
#
# Install does this in the target prefix:
#   1. imports the .reg       -> creates the HKLM\...\Enum\USB#VID_0EB7... key
#   2. copies fwfilterusb.sys -> C:\windows\system32\drivers\
#   3. registers a service    -> Group=WinePlugPlay, Start=2 (auto)
#   4. sets DeviceKey         -> points the driver at the key from (1)
#
# The driver loads automatically on the next wineboot (Start=2), so no
# explicit StartService is needed.
#
# Usage:
#   ./setup_ftec_fwfilter.sh [--prefix DIR | --proton APPID]
#                            [--arch x64|i386] [--reg FILE] [--sys FILE]
#                            [--data-dir DIR] [--pid PID]
#                            [--install [--rim PRESET] | --rim PRESET | --uninstall]
#                            [--list]
#
# Standalone wine uses --prefix DIR (or $WINEPREFIX). Proton prefixes are
# handled via protontricks (APPID = Steam app id).
#
# The wheel's PID is auto-detected from the connected USB devices (Fanatec
# VID 0eb7, via lsusb) and patched into the registry template. Use --pid to
# override or --list to show detected wheels.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROG="$(basename "$0")"
SVC='HKLM\SYSTEM\CurrentControlSet\Services\fwfilterusb'

ARCH="x64"
PREFIX=""
PROTON_APPID=""
PID_OVERRIDE=""
RIM=""
RIM_GIVEN=0
REG_GIVEN=0
SYS_GIVEN=0
ARCH_GIVEN=0
INSTALL=0
LIST=0
UNINSTALL=0
APPLY=0       # internal: run only the wine-side install steps (used under protontricks)
APPLY_RIM=0   # internal: run only the wine-side rim switch (used under protontricks)
EFF_REG=""  # PID-patched copy of $REG (removed on exit)
EFF_PID=""  # PID baked into $EFF_REG by prepare_reg

usage() {
    sed -n '2,/^$/p' "$HERE/$PROG" | sed 's/^# \?//'
    echo "Modes (default when none is given: --install):"
    echo "  --install       full install: import registry template, copy driver,"
    echo "                  register service (honors --rim, default 9rgb)"
    echo "  --rim PRESET    switch RimType on an already-installed prefix;"
    echo "                  with --install it selects the install-time preset"
    echo "  --uninstall     remove the driver, service key and hardware key"
    echo "Options:"
    echo "  Data files default to the script's directory, or \$FWFILTERUSB_DIR,"
    echo "  or /usr/share/fwfilterusb when installed system-wide."
    echo "  --prefix DIR    target Wine prefix (default: \$WINEPREFIX)"
    echo "  --proton APPID  target Proton prefix via protontricks"
    echo "  --arch ARCH     driver arch: x64 (default) or i386"
    echo "  --reg FILE      registry template (default: DATA_DIR/ftec_fwfilter.reg)"
    echo "  --sys FILE      driver binary (default: DATA_DIR/fwfilterusb[-x64].sys)"
    echo "  --data-dir DIR  data directory (default: script dir, else \$FWFILTERUSB_DIR,"
    echo "                  else /usr/share/fwfilterusb)"
    echo "  --pid PID       wheel product ID (default: auto-detect via lsusb;"
    echo "                  in --rim switch mode must match the installed key)"
    echo "  --rim PRESET    rev-LED rim preset, overwrites only RimType:"
    echo "                    9rgb       9 RGB rev LEDs (RimType 0x15)"
    echo "                    single-rgb single RGB rev LED, e.g. CSL Elite WRC"
    echo "                               (RimType 0x0F)"
    echo "                    9mono      9 non-RGB rev LEDs, e.g. Podium BMW M4 GT3"
    echo "                               (RimType 0x14)"
    echo "  --list          list detected Fanatec wheels and exit"
    echo "  -h, --help      show this help"
}

# --- argument parsing -------------------------------------------------------
# Data files live next to the script in a checkout, or in
# /usr/share/fwfilterusb when installed system-wide (ebuild).
DATA_DIR_OVERRIDE=""
if [[ -f "$HERE/ftec_fwfilter.reg" ]]; then
    DATA_DIR="$HERE"
else
    DATA_DIR="${FWFILTERUSB_DIR:-/usr/share/fwfilterusb}"
fi
REG=""
SYS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)   PREFIX="${2:-}"; shift 2 ;;
        --proton)   PROTON_APPID="${2:-}"; shift 2 ;;
        --arch)     ARCH="${2:-}"; ARCH_GIVEN=1; shift 2 ;;
        --reg)      REG="${2:-}"; REG_GIVEN=1; shift 2 ;;
        --sys)      SYS="${2:-}"; SYS_GIVEN=1; shift 2 ;;
        --data-dir) DATA_DIR_OVERRIDE="${2:-}"; shift 2 ;;
        --pid)      PID_OVERRIDE="${2:-}"; shift 2 ;;
        --rim)      RIM="${2:-}"; RIM_GIVEN=1; shift 2 ;;
        --install)  INSTALL=1; shift ;;
        --list)     LIST=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --apply)    APPLY=1; shift ;;   # internal (protontricks context)
        --apply-rim) APPLY_RIM=1; shift ;;   # internal (protontricks context)
        -h|--help)  usage; exit 0 ;;
        *) echo "error: unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
done

[[ -n "$DATA_DIR_OVERRIDE" ]] && DATA_DIR="$DATA_DIR_OVERRIDE"
case "$ARCH" in
    x64)  SYS="${SYS:-$DATA_DIR/fwfilterusb-x64.sys}" ;;
    i386) SYS="${SYS:-$DATA_DIR/fwfilterusb.sys}" ;;
    *) echo "error: --arch must be x64 or i386" >&2; exit 1 ;;
esac
REG="${REG:-$DATA_DIR/ftec_fwfilter.reg}"
RIM="${RIM:-9rgb}"

if [[ -n "$PREFIX" && -n "$PROTON_APPID" ]]; then
    echo "error: --prefix and --proton are mutually exclusive" >&2
    exit 1
fi

if [[ "$INSTALL" -eq 1 && "$UNINSTALL" -eq 1 ]]; then
    echo "error: --install and --uninstall are mutually exclusive" >&2
    exit 1
fi

if [[ -n "$PID_OVERRIDE" && ! "$PID_OVERRIDE" =~ ^[0-9A-Fa-f]{4}$ ]]; then
    echo "error: --pid must be 4 hex digits (e.g. 0020)" >&2
    exit 1
fi
PID_OVERRIDE="${PID_OVERRIDE^^}"

# Map the rim preset to the RimType DWORD written onto the hardware key.
# (Found via ACE: RimType 0x0F selects FSCmdLedRGBSingleTLC, 0x15 leaves the
# CSL Elite WRC LED dark; 0x14 selects FSCmdLedRevsWheelRim, the non-RGB
# 9-LED strip driver, via FUN_142dcd3b0.)
case "$RIM" in
    9rgb)      RIMTYPE="0x15" ;;
    single-rgb) RIMTYPE="0x0f" ;;
    9mono)     RIMTYPE="0x14" ;;
    *) echo "error: --rim must be 9rgb, single-rgb or 9mono" >&2; exit 1 ;;
esac

# Mode: bare call (or --install) = full install; lone --rim = RimType switch
# on an already-installed prefix; --uninstall removes everything.
MODE="install"
if [[ "$UNINSTALL" -eq 1 ]]; then
    # Internal --apply calls are machine-constructed (the outer layer already
    # validated); never trip them on a forwarded --rim.
    if [[ "$RIM_GIVEN" -eq 1 && "$APPLY" -eq 0 && "$APPLY_RIM" -eq 0 ]]; then
        echo "error: --rim cannot be combined with --uninstall" >&2
        exit 1
    fi
    MODE="uninstall"
elif [[ "$RIM_GIVEN" -eq 1 && "$INSTALL" -eq 0 && "$APPLY" -eq 0 && "$APPLY_RIM" -eq 0 ]]; then
    MODE="rim"
fi

if [[ "$MODE" == "rim" ]]; then
    [[ "$REG_GIVEN" -eq 1 ]] && { echo "error: --reg has no effect with --rim alone (add --install to reinstall)" >&2; exit 1; }
    [[ "$SYS_GIVEN" -eq 1 ]] && { echo "error: --sys has no effect with --rim alone (add --install to reinstall)" >&2; exit 1; }
    [[ "$ARCH_GIVEN" -eq 1 ]] && { echo "error: --arch has no effect with --rim alone (add --install to reinstall)" >&2; exit 1; }
fi

trap '[[ -n "$EFF_REG" ]] && rm -f "$EFF_REG"' EXIT

# --- Fanatec wheel detection (host USB, VID 0eb7) -------------------------------
fanatec_usb_lines() {
    command -v lsusb >/dev/null 2>&1 || return 0
    local out
    out="$(lsusb -d 0eb7: 2>/dev/null || true)"
    if [[ -z "$out" ]]; then
        out="$(lsusb 2>/dev/null | grep -i ' 0eb7:' || true)"
    fi
    printf '%s' "$out"
}

list_fanatec_wheels() {
    if ! command -v lsusb >/dev/null 2>&1; then
        echo "error: lsusb not found" >&2
        exit 1
    fi
    local out
    out="$(fanatec_usb_lines)"
    if [[ -z "$out" ]]; then
        echo "No Fanatec (VID 0eb7) USB devices detected."
        exit 1
    fi
    printf '%s\n' "$out"
}

# Echo the PID to use: --pid override, else the single connected wheel,
# else ask the user (interactive) or fall back to the template's PID.
resolve_pid() {
    if [[ -n "$PID_OVERRIDE" ]]; then
        echo "$PID_OVERRIDE"
        return 0
    fi
    local -a lines=() uniq=()
    if command -v lsusb >/dev/null 2>&1; then
        local l p seen q
        mapfile -t lines < <(fanatec_usb_lines)
        for l in ${lines[@]+"${lines[@]}"}; do
            p="$(sed -n 's/.*ID [0-9a-fA-F]\{1,\}:\([0-9a-fA-F]\{4\}\).*/\1/p' <<<"$l" | tr 'a-f' 'A-F')"
            [[ -n "$p" ]] || continue
            seen=0
            for q in ${uniq[@]+"${uniq[@]}"}; do
                [[ "$p" == "$q" ]] && { seen=1; break; }
            done
            [[ "$seen" -eq 0 ]] && uniq+=("$p")
        done
    else
        echo "warning: lsusb not found; using PID from $REG" >&2
    fi
    if [[ "${#uniq[@]}" -eq 1 ]]; then
        echo "Detected wheel : ${lines[0]}" >&2
        echo "${uniq[0]}"
        return 0
    fi
    if [[ "${#uniq[@]}" -gt 1 ]]; then
        echo "error: multiple Fanatec devices detected; select one with --pid:" >&2
        printf '  %s\n' "${lines[@]}" >&2
        exit 1
    fi
    local tpl
    tpl="$(grep -o 'PID_[0-9A-Fa-f]\{4\}' "$REG" 2>/dev/null | head -1 | cut -d_ -f2 | tr 'a-f' 'A-F' || true)"
    if [[ -t 0 ]]; then
        query_pid_interactive "$tpl"
        return 0
    fi
    if [[ -z "$tpl" ]]; then
        echo "error: no Fanatec wheel detected and no PID in $REG (use --pid XXXX)" >&2
        exit 1
    fi
    echo "warning: no Fanatec wheel detected; using template PID $tpl" >&2
    echo "$tpl"
}

# Prompt for the PID on a terminal; empty input accepts the default.
# Only stdout is the chosen PID (prompt goes to stderr).
query_pid_interactive() {
    local default="$1" input=""
    echo "No Fanatec wheel detected via lsusb." >&2
    while true; do
        if ! IFS= read -r -p "Enter wheel PID [${default:-none}]: " input; then
            echo >&2
            echo "error: aborted" >&2
            exit 1
        fi
        input="${input^^}"
        [[ -z "$input" ]] && input="$default"
        if [[ "$input" =~ ^[0-9A-Fa-f]{4}$ ]]; then
            echo "$input"
            return 0
        fi
        [[ -z "$input" ]] && { echo "error: no PID given" >&2; exit 1; }
        echo "  (must be 4 hex digits, e.g. 0020)" >&2
    done
}

# Patch the detected/overridden PID into a temp copy of the template.
# Sets $REG to the patched copy (cleaned up on exit) and $EFF_PID to the PID.
prepare_reg() {
    [[ -f "$REG" ]] || { echo "error: $REG not found" >&2; exit 1; }
    local pid
    pid="$(resolve_pid)"
    EFF_REG="$(mktemp "${TMPDIR:-/tmp}/fwfilterusb-reg.XXXXXX")"
    sed "/^\\[/ s/PID_[0-9A-Fa-f]\\{4\\}/PID_${pid}/" "$REG" > "$EFF_REG"
    REG="$EFF_REG"
    EFF_PID="$pid"
    echo "Wheel PID    : $pid"
}

# --- registry key parsing ----------------------------------------------------
# The .reg has a line like:
#   [HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\USB#VID_0EB7&PID_0020#001#...]
parse_rel_key() {
    local line
    line="$(grep -E '^\[HKEY_LOCAL_MACHINE' "$REG" | head -1 | tr -d '[]')"
    if [[ -z "$line" ]]; then
        echo "error: no [HKEY_LOCAL_MACHINE...] key in $REG" >&2
        exit 1
    fi
    # strip the HKEY_LOCAL_MACHINE\ prefix -> relative path the driver expects
    echo "${line#HKEY_LOCAL_MACHINE\\}"
}

# --- wine-side steps (run with the right `wine` on PATH) ----------------------
# Resolve the hardware key for --rim switch mode (full HKLM\... path) from
# the DeviceKey service value written at install time.
resolve_hw_key() {
    local devkey="" key=""
    devkey="$(wine reg query "$SVC" /v DeviceKey 2>/dev/null | tr -d '\r' | awk '$1 == "DeviceKey" && $2 == "REG_SZ" { print $3 }' || true)"
    if [[ -z "$devkey" ]]; then
        echo "error: no installed Fanatec hardware key found (run --install first)" >&2
        exit 1
    fi
    key="HKLM\\${devkey}"
    if [[ -n "$PID_OVERRIDE" && "$key" != *"PID_${PID_OVERRIDE}"* ]]; then
        echo "error: installed DeviceKey $key does not match --pid $PID_OVERRIDE" >&2
        exit 1
    fi
    echo "$key"
}

# Overwrite only RimType on an already-installed hardware key (full HKLM\...).
registry_set_rim() {
    local hw_key="$1"
    local old=""
    old="$(wine reg query "$hw_key" /v RimType 2>/dev/null | tr -d '\r' | awk '$1 == "RimType" && $2 == "REG_DWORD" { print $3 }' || true)"
    wine reg add "$hw_key" /v RimType /t REG_DWORD /d "$RIMTYPE" /f
    if [[ -n "$old" ]]; then
        echo "RimType      : $old -> $RIMTYPE ($RIM)"
    else
        echo "Rim          : $RIM (RimType $RIMTYPE)"
    fi
}

registry_install() {
    local rel_key="$1"
    local hw_key="HKLM\\${rel_key}"
    wine regedit /s "$REG"
    # Rim preset: overwrite only RimType on the hardware key.
    wine reg add "$hw_key" /v RimType /t REG_DWORD /d "$RIMTYPE" /f
    wine reg add "$SVC" /f >/dev/null 2>&1 || true
    wine reg add "$SVC" /v Type         /t REG_DWORD /d 0x1 /f
    wine reg add "$SVC" /v Start        /t REG_DWORD /d 0x2 /f
    wine reg add "$SVC" /v ErrorControl /t REG_DWORD /d 0x1 /f
    wine reg add "$SVC" /v Group        /t REG_SZ    /d "WinePlugPlay" /f
    wine reg add "$SVC" /v ObjectName   /t REG_SZ    /d "LocalSystem" /f
    wine reg add "$SVC" /v ImagePath    /t REG_SZ    /d 'C:\windows\system32\drivers\fwfilterusb.sys' /f
    wine reg add "$SVC" /v DeviceKey    /t REG_SZ    /d "$rel_key" /f
}

registry_uninstall() {
    wine reg delete "$SVC" /f >/dev/null 2>&1 || true
    # Remove the Fanatec hardware key(s) imported from the .reg template.
    # The key is a literal `USB#VID_0EB7...` sibling of `USB`, and the
    # installed PID/serial may differ from the template, so delete every
    # VID_0EB7 match instead of resolving a single PID (also avoids
    # prompting when no wheel is connected).
    local k
    while IFS= read -r k; do
        k="${k%$'\r'}"
        [[ -n "$k" ]] || continue
        wine reg delete "$k" /f >/dev/null 2>&1 || true
    done < <(wine reg query 'HKLM\SYSTEM\CurrentControlSet\Enum' 2>/dev/null | grep -i '^HKEY.*VID_0EB7' || true)
}

drivers_dir() {
    echo "$1/drive_c/windows/system32/drivers"
}

# --- internal entry point: ambient wine is already correct --------------------
if [[ "$LIST" -eq 1 ]]; then
    list_fanatec_wheels
    exit 0
fi

if [[ "$APPLY" -eq 1 ]]; then
    if [[ "$UNINSTALL" -eq 1 ]]; then
        registry_uninstall
        echo "Driver, service key and hardware key removed."
    else
        prepare_reg
        registry_install "$(parse_rel_key)"
        echo "Rim          : $RIM (RimType $RIMTYPE)"
        echo "Installed. The driver auto-loads on the next wineboot."
    fi
    exit 0
fi

if [[ "$APPLY_RIM" -eq 1 ]]; then
    hw_key="$(resolve_hw_key)" || exit 1
    registry_set_rim "$hw_key"
    echo "Switched. Restart the game to apply."
    exit 0
fi

# Ask protontricks for the game's prefix (its WINEPREFIX env).
# This respects libraryfolders.vdf, STEAM_DIR, Flatpak/Snap installs, etc.
proton_prefix() {
    proton_run -c 'printf %s "$WINEPREFIX"' "$1" 2>/dev/null | tr -d '\r\n'
}

# Run protontricks with a sanitized environment so the inner commands always
# use the game's Proton wine: protontricks honors a user-provided $WINE /
# $WINESERVER (e.g. from a wine version manager), which would run system wine
# against a Proton prefix. (WINELOADER is derived from WINE by protontricks;
# WINEPREFIX/WINEDLLPATH are force-set by it, so they need no handling.)
proton_run() {
    env -u WINE -u WINESERVER protontricks "$@"
}

# --- Proton prefix resolution --------------------------------------------------
if [[ -n "$PROTON_APPID" ]]; then
    if ! command -v protontricks >/dev/null 2>&1; then
        echo "error: protontricks not found (needed for --proton)" >&2
        exit 1
    fi
    PFX="$(proton_prefix "$PROTON_APPID" || true)"
    if [[ -z "$PFX" || ! -d "$PFX" ]]; then
        echo "error: Proton prefix for APPID $PROTON_APPID not found" >&2
        echo "  (protontricks could not resolve it; run the game once first)" >&2
        exit 1
    fi
    if [[ "$MODE" == "rim" ]]; then
        # RimType switch only: registry steps run under the game's Proton
        # wine; no template import, no driver copy.
        # shellcheck disable=SC2086
        inner="$(printf '%q ' "$HERE/$PROG" --apply-rim --rim "$RIM")"
        [[ -n "$PID_OVERRIDE" ]] && inner="$inner --pid $PID_OVERRIDE"
        proton_run -c "$inner" "$PROTON_APPID"
        echo "Done (Proton APPID $PROTON_APPID)."
        exit 0
    fi
    # File copy happens on the host; registry steps run under the game's
    # Proton wine via proton_run (immune to any ambient $WINE setting).
    # The PID is resolved once here, baked into the effective .reg and
    # forwarded via --pid, so the inner --apply never prompts a second time.
    if [[ "$UNINSTALL" -eq 0 ]]; then
        [[ -f "$SYS" ]] || { echo "error: $SYS not found. Build it first: make $ARCH" >&2; exit 1; }
        prepare_reg
    fi
    # shellcheck disable=SC2086
    inner="$(printf '%q ' "$HERE/$PROG" --apply --reg "$REG")"
    # Forward --rim for installs only; the uninstall branch ignores it and an
    # explicit --rim + --uninstall combo is rejected.
    [[ "$UNINSTALL" -eq 0 ]] && inner="$inner --rim $RIM"
    [[ -n "$EFF_PID" ]] && inner="$inner --pid $EFF_PID"
    if [[ "$UNINSTALL" -eq 1 ]]; then
        inner="$inner --uninstall"
        rm -f "$PFX/drive_c/windows/system32/drivers/fwfilterusb.sys"
    else
        mkdir -p "$(drivers_dir "$PFX")"
        cp -f "$SYS" "$(drivers_dir "$PFX")/fwfilterusb.sys"
        echo "Driver       : $SYS -> $(drivers_dir "$PFX")/fwfilterusb.sys"
    fi
    # shellcheck disable=SC2086
    proton_run -c "$inner" "$PROTON_APPID"
    echo "Done (Proton APPID $PROTON_APPID)."
    exit 0
fi

# --- standalone prefix ---------------------------------------------------------
PREFIX="${PREFIX:-${WINEPREFIX:-}}"
if [[ -z "$PREFIX" ]]; then
    echo "error: no target prefix (use --prefix DIR, --proton APPID, or set WINEPREFIX)" >&2
    exit 1
fi
# The registry steps below call ambient `wine`, so point it at the target.
export WINEPREFIX="$PREFIX"

if [[ "$UNINSTALL" -eq 1 ]]; then
    rm -f "$(drivers_dir "$PREFIX")/fwfilterusb.sys"
    registry_uninstall
    echo "Driver, service key and hardware key removed from $PREFIX."
    exit 0
fi

if [[ "$MODE" == "rim" ]]; then
    hw_key="$(resolve_hw_key)" || exit 1
    registry_set_rim "$hw_key"
    echo "Done. Restart the game to apply."
    exit 0
fi

[[ -f "$SYS" ]] || { echo "error: $SYS not found. Build it first: make $ARCH" >&2; exit 1; }
prepare_reg
REL_KEY="$(parse_rel_key)"
echo "Hardware key : $REL_KEY"
echo "Driver       : $SYS"
echo "Rim          : $RIM (RimType $RIMTYPE)"
mkdir -p "$(drivers_dir "$PREFIX")"
cp -f "$SYS" "$(drivers_dir "$PREFIX")/fwfilterusb.sys"
registry_install "$REL_KEY"
echo "Done. Next time you start wine the driver will be active."
