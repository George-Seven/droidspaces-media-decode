#!/usr/bin/env bash
# Configure a native Jellyfin systemd service to use the installed VA-API driver.
set -euo pipefail

unit=jellyfin.service
dropin_dir=/etc/systemd/system/${unit}.d
dropin=$dropin_dir/90-droidspaces-vaapi.conf
marker='# Managed by configure-jellyfin-vaapi.sh'
driver_dir=/usr/lib/aarch64-linux-gnu/dri
mode=install

fail() { printf 'configure-jellyfin-vaapi: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: configure-jellyfin-vaapi.sh [--driver-dir DIR | --remove]

Install the systemd configuration as root, then restart Jellyfin if it is
running. --remove removes this script's configuration. The default driver
directory is /usr/lib/aarch64-linux-gnu/dri.
EOF
}

case "${1:-}" in
    '') [[ $# == 0 ]] || fail 'unexpected empty argument' ;;
    --driver-dir)
        [[ $# == 2 && -n $2 ]] || fail '--driver-dir requires a directory'
        driver_dir=$2
        ;;
    --remove) [[ $# == 1 ]] || fail '--remove takes no arguments'; mode=remove ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

[[ $EUID == 0 ]] || fail 'run this script with sudo'
[[ $(systemctl show "$unit" --property=LoadState --value) == loaded ]] ||
    fail "$unit is not installed or is masked"

if [[ -e $dropin || -L $dropin ]]; then
    [[ ! -L $dropin && -f $dropin ]] || fail "refusing to modify $dropin"
    IFS= read -r first_line < "$dropin" || fail "cannot read $dropin"
    [[ $first_line == "$marker" ]] || fail "refusing to replace unmanaged file $dropin"
elif [[ $mode == remove ]]; then
    printf 'No managed drop-in at %s\n' "$dropin"
    exit 0
fi

if [[ $mode == remove ]]; then
    rm -- "$dropin"
    printf 'Removed %s\n' "$dropin"
else
    driver_dir=$(realpath -e -- "$driver_dir") || fail 'driver directory does not exist'
    [[ $driver_dir =~ ^/[a-zA-Z0-9_./-]+$ ]] ||
        fail 'use a driver directory with no spaces or special characters'
    [[ -f $driver_dir/msm_drm_drv_video.so ]] ||
        fail "msm_drm_drv_video.so is missing from $driver_dir"
    getent group droidspaces-gpu >/dev/null || fail 'the droidspaces-gpu group is missing'
    for device in /dev/dri/renderD128 /dev/video32 /dev/video33 /dev/dma_heap/system; do
        [[ -c $device ]] || fail "required device is missing: $device"
        [[ $(stat -Lc %G -- "$device") == droidspaces-gpu ]] ||
            fail "$device is not owned by the droidspaces-gpu group"
    done
    [[ $(systemctl show "$unit" --property=PrivateDevices --value) != yes ]] ||
        fail "$unit has PrivateDevices=yes and cannot see the hardware devices"

    install -d -m 0755 -- "$dropin_dir"
    tmp=$(mktemp "$dropin_dir/.droidspaces-vaapi.XXXXXXXX")
    trap 'if [[ -e $tmp ]]; then rm -- "$tmp"; fi' EXIT
    cat > "$tmp" <<EOF
$marker
[Service]
Environment="LIBVA_DRIVER_NAME=msm_drm"
Environment="LIBVA_DRIVERS_PATH=$driver_dir"
SupplementaryGroups=droidspaces-gpu
EOF
    chmod 0644 -- "$tmp"
    mv -fT -- "$tmp" "$dropin"
    printf 'Configured %s\n' "$dropin"
fi

systemctl daemon-reload
systemctl try-restart "$unit"
if ! systemctl is-active --quiet "$unit"; then
    printf '%s is stopped. Start it when ready with: sudo systemctl start %s\n' "$unit" "$unit"
fi

if [[ $mode == install ]]; then
    printf '%s\n' 'In Jellyfin Dashboard > Playback > Transcoding, select VA-API with device /dev/dri/renderD128.'
fi
