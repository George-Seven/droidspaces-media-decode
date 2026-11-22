#!/bin/sh
# Build an arm64 Debian package containing the full decode driver, GBM adapter
# and process-scoped Chrome launcher. Run this on the target arm64 system.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PACKAGE=droidspaces-msm-vaapi
VERSION=${1:-${DMD_PACKAGE_VERSION:-0.1.0}}
OUT_DIR=${2:-"$ROOT/dist"}
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

case "$VERSION" in
    ''|*[!A-Za-z0-9.+:~_-]*) echo "Invalid Debian version: $VERSION" >&2; exit 2 ;;
esac
[ "$ARCH" = arm64 ] || {
    echo "This package currently targets arm64; detected $ARCH." >&2
    exit 2
}

REAL_GBM=$(ldconfig -p | awk '/libgbm\.so\.1[[:space:]]/ && /=>/ { print $NF; exit }')
[ -n "$REAL_GBM" ] && [ -r "$REAL_GBM" ] || {
    echo 'Could not find the system libgbm.so.1. Install libgbm1 first.' >&2
    exit 1
}
REAL_GBM=$(readlink -f "$REAL_GBM")

BUILD_ROOT="$ROOT/build/deb"
DRIVER_BUILD="$BUILD_ROOT/driver"
GBM_BUILD="$BUILD_ROOT/gbm-compat"
STAGE="$BUILD_ROOT/root"
DEBIAN="$STAGE/DEBIAN"

mkdir -p "$OUT_DIR" "$DEBIAN"

echo 'Building full-featured msm_drm VA-API driver (AV1 + HEVC Main10)...'
make -C "$ROOT/vaapi-driver" BUILD="$DRIVER_BUILD" AV1=1 HEVC10=1 all

echo "Building GBM adapter against $REAL_GBM..."
make -C "$ROOT/vaapi-driver/gbm-compat" BUILD="$GBM_BUILD" REAL_GBM="$REAL_GBM" all

rm -rf "$STAGE"
mkdir -p "$DEBIAN" \
    "$STAGE/usr/lib/$MULTIARCH/dri" \
    "$STAGE/usr/lib/$MULTIARCH/droidspaces-gbm" \
    "$STAGE/usr/bin" \
    "$STAGE/usr/share/doc/$PACKAGE"

install -m 0644 "$DRIVER_BUILD/msm_drm_drv_video.so" \
    "$STAGE/usr/lib/$MULTIARCH/dri/msm_drm_drv_video.so"
install -m 0644 "$GBM_BUILD/libgbm.so.1" \
    "$STAGE/usr/lib/$MULTIARCH/droidspaces-gbm/libgbm.so.1"
sed "s|@MULTIARCH@|$MULTIARCH|g" "$ROOT/packaging/deb/chrome-msm-vaapi.in" \
    > "$STAGE/usr/bin/chrome-msm-vaapi"
chmod 0755 "$STAGE/usr/bin/chrome-msm-vaapi"
install -m 0644 "$ROOT/README.md" "$STAGE/usr/share/doc/$PACKAGE/README.md"
install -m 0644 "$ROOT/vaapi-driver/gbm-compat/README.md" \
    "$STAGE/usr/share/doc/$PACKAGE/gbm-compat.md"

sed "s/^Version:.*/Version: $VERSION/" "$ROOT/packaging/deb/control" > "$DEBIAN/control"
chmod 0644 "$DEBIAN/control"

OUTPUT="$OUT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUTPUT"
echo "Created $OUTPUT"
