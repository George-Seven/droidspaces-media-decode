#!/usr/bin/env bash
set -euo pipefail

checkout() {
    local repo=$1 commit=$2 dir=$3
    git init -q "$dir"
    git -C "$dir" fetch --quiet --depth=1 "https://github.com/jellyfin/$repo.git" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD
    test "$(git -C "$dir" rev-parse HEAD)" = "$commit"
}

ffmpeg_environment() {
    cd /ffmpeg
    export SOURCE_DIR=/ffmpeg TARGET_DIR=/usr/lib/jellyfin-ffmpeg
    export DPKG_INSTALL_LIST=/ffmpeg/debian/jellyfin-ffmpeg8.install
    export ARCH=arm64 LLVM_VER=21
    export DEB_BUILD_OPTIONS="noddebs parallel=$BUILD_JOBS"
    export PATH="$TARGET_DIR/bin:$PATH"
    export PKG_CONFIG_PATH="$TARGET_DIR/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig"
    export LD_LIBRARY_PATH="$TARGET_DIR/lib:$TARGET_DIR/lib/mfx:$TARGET_DIR/lib/xorg"
    export LDFLAGS="-Wl,--disable-new-dtags -Wl,-rpath=$TARGET_DIR/lib -L$TARGET_DIR/lib -lm"
    export CPPFLAGS="-I$TARGET_DIR/include" CFLAGS="-I$TARGET_DIR/include" CXXFLAGS="-I$TARGET_DIR/include"
    export CMAKE_POLICY_VERSION_MINIMUM=3.5
    export SOURCE_DATE_EPOCH
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
}

sync_shaderc_dependencies() {
    python3 - <<'PYTHON'
import runpy
import subprocess

parse_deps = runpy.run_path("utils/git-sync-deps")["parse_file_to_dict"]
for directory, dependency in sorted(parse_deps("DEPS")["deps"].items()):
    url, commit = dependency.rsplit("@", 1)
    subprocess.run(["git", "init", "--quiet", directory], check=True)
    git = ["git", "-C", directory]
    for attempt in range(3):
        result = subprocess.run(git + ["fetch", "--quiet", "--depth=1", url, commit])
        if result.returncode == 0:
            break
    result.check_returncode()
    subprocess.run(git + ["checkout", "--quiet", "--detach", "FETCH_HEAD"], check=True)
PYTHON
}

ffmpeg_dependency_block() {
    local first=$1 last=$2
    # Each block starts at a marker in the pinned upstream dependency recipe.
    awk -v first="$first" -v last="$last" '
        $0 == first { found = 1 }
        found && $0 == last { complete = 1; exit }
        found { print }
        END { if (!complete) exit 1 }
    ' docker-build.sh | sed 's/$(nproc)/$BUILD_JOBS/g; s|\./utils/git-sync-deps|sync_shaderc_dependencies|' > build-dependencies.sh
    export CROSS_PREFIX_OPT=aarch64-linux-gnu-
    export CROSS_OPT='--host=aarch64-linux-gnu CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++'
    export CMAKE_TOOLCHAIN_OPT="-DCMAKE_TOOLCHAIN_FILE=$SOURCE_DIR/toolchain-arm64.cmake"
    export MESON_CROSS_OPT="--cross-file=$SOURCE_DIR/cross-arm64.meson"
    # shellcheck disable=SC1091
    source build-dependencies.sh
}

case "$1" in
    prepare-server)
        checkout jellyfin-packaging 12a5fb713de032ee311b5ec9c2639293aa329665 /server
        # The upstream release workflow selects the v12.4 tag.
        checkout jellyfin 83d0d0e7f76e23e2e085c8ff67d28b772d35967d /server/jellyfin-server
        cd /server/jellyfin-server
        git apply /patches/jellyfin-v12.2-vaapi-rotation-metadata.patch
        git apply /patches/jellyfin-v12.2-hls-vfr-timestamps.patch
        touch /verified-server
        ;;
    prepare-ffmpeg)
        checkout jellyfin-ffmpeg 7250de8375a4959ac1c25c139ce9789fdac199fb /ffmpeg
        git -C /ffmpeg apply /patches/jellyfin-ffmpeg-8.1.3-rotate.patch
        touch /verified-ffmpeg
        ;;
    server)
        cd /server
        export SOURCE_DATE_EPOCH
        SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
        # Retain the official server installation and maintainer scripts.
        awk 'BEGIN {RS=""; ORS="\n\n"} NR==1 || /^Package: jellyfin-server\n/' debian/control \
            | sed '/^Build-Depends:/c\Build-Depends: debhelper (>= 10)' \
            | sed '/^ *npm | nodejs,/d' > debian/control.server
        mv debian/control.server debian/control
        sed -i '/^\s*cd jellyfin-web && npm ci/,+1d; s/^HOST_ARCH := .*/HOST_ARCH := arm64/' debian/rules
        rm debian/jellyfin-web.conffiles debian/jellyfin-web.install
        cat > debian/changelog <<CHANGELOG
jellyfin (12.4+ubu2604+dmd1) unstable; urgency=medium

  * Apply DroidSpaces rotation and HLS timestamp patches.

 -- DroidSpaces build <build@localhost>  $(date -u -d "@$SOURCE_DATE_EPOCH" -R)
CHANGELOG
        DEB_BUILD_OPTIONS=noddebs dpkg-buildpackage -b -us -uc
        mkdir /packages
        cp /jellyfin-server_12.4+ubu2604+dmd1_arm64.deb /packages/
        ;;
    ffmpeg-deps)
        ffmpeg_environment
        ffmpeg_dependency_block '    # ICONV' '    # SHADERC'
        ;;
    ffmpeg-shaderc)
        ffmpeg_environment
        ffmpeg_dependency_block '    # SHADERC' '    # MESA'
        ;;
    ffmpeg-graphics)
        ffmpeg_environment
        ffmpeg_dependency_block '    # MESA' '}'
        ffmpeg_dependency_block '    # RKMPP' '}'
        ;;
    ffmpeg)
        ffmpeg_environment
        {
            cat <<CHANGELOG
jellyfin-ffmpeg (8.1.3-2-resolute+dmd1) resolute; urgency=medium

  * Apply the DroidSpaces MOV rotation metadata fallback.

 -- DroidSpaces build <build@localhost>  $(date -u -d "@$SOURCE_DATE_EPOCH" -R)

CHANGELOG
            cat debian/changelog
        } > debian/changelog.patched
        mv debian/changelog.patched debian/changelog
        # The upstream ARM64 recipe assumes an AMD64 host and appends the
        # distro suffix. Use native Clang and the full changelog version.
        sed -i 's/clang:amd64/clang/' debian/control
        sed -i 's/^PACKAGE_VERSION := .*/PACKAGE_VERSION := $(ORIG_VERSION)/' debian/rules
        mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
        dpkg-buildpackage -b -rfakeroot -us -uc
        mkdir /packages
        cp /jellyfin-ffmpeg8_8.1.3-2-resolute+dmd1_arm64.deb /packages/
        ;;
    *) exit 2 ;;
esac
