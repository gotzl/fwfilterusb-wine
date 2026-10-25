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
#   5. sets SysfsPath         -> sysfs dir the driver mirrors values from
#      (as a z:\... path; Wine maps z: to /)
#
# The driver loads automatically on the next wineboot (Start=2), so no
# explicit StartService is needed. It syncs RimType/WheelType from that
# directory at startup and on SDK reload requests.
#
# Usage:
#   ./setup_ftec_fwfilter.sh [--prefix DIR | --proton APPID]
#                            [--arch x64|i386] [--reg FILE] [--sys FILE]
#                            [--data-dir DIR] [--pid PID]
#                            [--sysfs-path GLOB] [--install | --uninstall]
#                            [--list]
#
# Standalone wine uses --prefix DIR (or $WINEPREFIX). Proton prefixes are
# handled via protontricks (APPID = Steam app id).
#
# The wheel's PID is auto-detected from the connected USB devices (Fanatec
# VID 0eb7, via lsusb) and patched into the registry template. Use --pid to
# override or --list to show detected wheels.

set -euo pipefail

# Wine's debug channels are chatty during prefix setup; users can still opt in.
export WINEDEBUG="${WINEDEBUG:--all}"

# fsync/esync print an unconditional startup banner; drop just that.
filter_wine_noise() {
    grep -v -F -e 'fsync: up and running.' -e 'esync: up and running.' >&2
}
wine() { command wine "$@" 2> >(filter_wine_noise); }

HERE="$(cd "$(dirname "$0")" && pwd)"
PROG="$(basename "$0")"
SVC='HKLM\SYSTEM\CurrentControlSet\Services\fwfilterusb'
VID='0EB7'
# hid_fanatec sysfs dir; the attributes live in per-interface subdirs.
SYSFS_SYNC_DIR='/sys/module/hid_fanatec/drivers/hid:fanatec'

ARCH="x64"
PREFIX=""
PROTON_APPID=""
PID_OVERRIDE=""
SYSFS_PATH_OVERRIDE=""
INSTALL=0
LIST=0
UNINSTALL=0
APPLY=0       # internal: run only the wine-side install steps (used under protontricks)
EFF_REG=""  # PID-patched copy of $REG (removed on exit)
EFF_PID=""  # PID baked into $EFF_REG by prepare_reg

usage() {
    sed -n '2,/^$/p' "$HERE/$PROG" | sed 's/^# \?//'
    echo "Modes (default when none is given: --install):"
    echo "  --install       full install: import registry template, copy driver,"
    echo "                  register service, set up sysfs sync"
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
    echo "  --pid PID       wheel product ID (default: auto-detect via lsusb)"
    echo "  --sysfs-path GLOB"
    echo "                  sysfs device dir to sync from (absolute unix path, at"
    echo "                  most one '*'; default:"
    echo "                  $SYSFS_SYNC_DIR/0003:<VID>:<PID>.*)"
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
        --arch)     ARCH="${2:-}"; shift 2 ;;
        --reg)      REG="${2:-}"; shift 2 ;;
        --sys)      SYS="${2:-}"; shift 2 ;;
        --data-dir) DATA_DIR_OVERRIDE="${2:-}"; shift 2 ;;
        --pid)      PID_OVERRIDE="${2:-}"; shift 2 ;;
        --sysfs-path) SYSFS_PATH_OVERRIDE="${2:-}"; shift 2 ;;
        --install)  INSTALL=1; shift ;;
        --list)     LIST=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --apply)    APPLY=1; shift ;;   # internal (protontricks context)
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

if [[ -n "$SYSFS_PATH_OVERRIDE" ]]; then
    if [[ "$SYSFS_PATH_OVERRIDE" != /* ]]; then
        echo "error: --sysfs-path must be an absolute unix path (starting with /)" >&2
        exit 1
    fi
    stars="${SYSFS_PATH_OVERRIDE//[^*]/}"
    if [[ "${#stars}" -gt 1 ]]; then
        echo "error: --sysfs-path may contain at most one '*'" >&2
        exit 1
    fi
fi


# --- sysfs sync ---------------------------------------------------------------
# Wine maps z: to / in every prefix, so the unix path is reachable as z:\...

# Build the DOS SysfsPath glob for PID $1 and echo it (empty on failure).
sysfs_dos_pattern() {
    local unix_pat="${SYSFS_PATH_OVERRIDE:-$SYSFS_SYNC_DIR/0003:$VID:$1.*}"
    # A fresh prefix gets the default z: -> / mapping on first wine start.
    if [[ -d "${WINEPREFIX:-}/dosdevices" ]]; then
        [[ "$(readlink "${WINEPREFIX}/dosdevices/z:" 2>/dev/null)" == / ]] || return 1
    fi
    printf 'z:%s\n' "${unix_pat//\//\\}"
}

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
registry_install() {
    local rel_key="$1"
    local sysfs_dos="" img_esc dev_esc sys_esc
    local image='C:\windows\system32\drivers\fwfilterusb.sys'

    # Point the driver at the sysfs dir; template values stay as fallback.
    if sysfs_dos="$(sysfs_dos_pattern "$EFF_PID")"; then
        echo "Sysfs sync   : $sysfs_dos (from $SYSFS_SYNC_DIR)"
    else
        echo "warning: no sysfs drive mapping; registry values stay static (configure --sysfs-path)" >&2
    fi

    img_esc="${image//\\/\\\\}"
    dev_esc="${rel_key//\\/\\\\}"
    sys_esc="${sysfs_dos//\\/\\\\}"

    cat >> "$REG" <<'EOF'

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\fwfilterusb]
"Type"=dword:00000001
"Start"=dword:00000002
"ErrorControl"=dword:00000001
"Group"="WinePlugPlay"
"ObjectName"="LocalSystem"
EOF
    printf '"ImagePath"="%s"\n' "$img_esc" >> "$REG"
    printf '"DeviceKey"="%s"\n' "$dev_esc" >> "$REG"
    [[ -z "$sysfs_dos" ]] || printf '"SysfsPath"="%s"\n' "$sys_esc" >> "$REG"

    echo "Importing registry template and service settings..."
    wine regedit /s "$REG" >/dev/null
}

registry_uninstall() {
    echo "Removing driver service and hardware keys..."
    wine reg delete "$SVC" /f >/dev/null 2>&1 || true
    # Remove all imported Fanatec hardware keys (every Vid_0eb7 match, not
    # just the template's; avoids prompting when no wheel is connected).
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
        echo "Installed. The driver auto-loads on the next wineboot."
    fi
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
    env -u WINE -u WINESERVER protontricks "$@" 2> >(filter_wine_noise)
}

# --- Proton prefix resolution --------------------------------------------------
if [[ -n "$PROTON_APPID" ]]; then
    if ! command -v protontricks >/dev/null 2>&1; then
        echo "error: protontricks not found (needed for --proton)" >&2
        exit 1
    fi
    echo "Resolving Proton prefix..."
    PFX="$(proton_prefix "$PROTON_APPID" || true)"
    if [[ -z "$PFX" || ! -d "$PFX" ]]; then
        echo "error: Proton prefix for APPID $PROTON_APPID not found" >&2
        echo "  (protontricks could not resolve it; run the game once first)" >&2
        exit 1
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
    # Forward --sysfs-path on installs only (uninstall ignores it).
    [[ "$UNINSTALL" -eq 0 && -n "$SYSFS_PATH_OVERRIDE" ]] && \
        inner="$inner --sysfs-path $(printf '%q' "$SYSFS_PATH_OVERRIDE")"
    [[ -n "$EFF_PID" ]] && inner="$inner --pid $EFF_PID"
    if [[ "$UNINSTALL" -eq 1 ]]; then
        inner="$inner --uninstall"
        echo "Removing driver binary..."
        rm -f "$PFX/drive_c/windows/system32/drivers/fwfilterusb.sys"
    else
        echo "Copying driver..."
        mkdir -p "$(drivers_dir "$PFX")"
        cp -f "$SYS" "$(drivers_dir "$PFX")/fwfilterusb.sys"
        echo "Driver       : $SYS -> $(drivers_dir "$PFX")/fwfilterusb.sys"
    fi
    # shellcheck disable=SC2086
    echo "Updating registry..."
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
    echo "Removing driver binary..."
    rm -f "$(drivers_dir "$PREFIX")/fwfilterusb.sys"
    registry_uninstall
    echo "Driver, service key and hardware key removed from $PREFIX."
    exit 0
fi

[[ -f "$SYS" ]] || { echo "error: $SYS not found. Build it first: make $ARCH" >&2; exit 1; }
prepare_reg
REL_KEY="$(parse_rel_key)"
echo "Hardware key : $REL_KEY"
echo "Driver       : $SYS"
echo "Copying driver..."
mkdir -p "$(drivers_dir "$PREFIX")"
cp -f "$SYS" "$(drivers_dir "$PREFIX")/fwfilterusb.sys"
registry_install "$REL_KEY"
echo "Done. Next time you start wine the driver will be active."
