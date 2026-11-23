# syntax=docker/dockerfile:1
ARG BASE_IMAGE=ubuntu:26.04@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db
FROM ${BASE_IMAGE} AS tools
ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC
RUN test "$(dpkg --print-architecture)" = arm64 && apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git

FROM tools AS sources
COPY packaging/jellyfin/build-in-container.sh /usr/local/bin/build-jellyfin
COPY patches/ /patches/
COPY packaging/jellyfin/jellyfin-ffmpeg-8.1.3-rotate.patch /patches/

FROM sources AS server-source
RUN build-jellyfin prepare-server

FROM sources AS ffmpeg-source
RUN build-jellyfin prepare-ffmpeg

FROM tools AS patch-check
COPY --from=server-source /verified-server /verified-server
COPY --from=ffmpeg-source /verified-ffmpeg /verified-ffmpeg
RUN --mount=from=ffmpeg-source,source=/ffmpeg,target=/ffmpeg \
    --mount=type=bind,source=packaging/jellyfin/jellyfin-ffmpeg-8.1.3-vfr-vaapi.patch,target=/tmp/vfr-vaapi.patch \
    git -C /ffmpeg apply --check /tmp/vfr-vaapi.patch

FROM tools AS server-build
RUN apt-get install -y --no-install-recommends build-essential debhelper dotnet-sdk-10.0 \
    libfontconfig-dev libfreetype-dev libicu-dev libkrb5-3 libssl-dev zlib1g-dev
COPY --from=server-source /server /server
COPY --from=sources /usr/local/bin/build-jellyfin /usr/local/bin/
RUN --mount=type=cache,target=/root/.nuget/packages,sharing=locked build-jellyfin server

FROM tools AS ffmpeg-build
ARG BUILD_JOBS=4
ENV BUILD_JOBS=${BUILD_JOBS}
RUN apt-get install -y --no-install-recommends build-essential devscripts debhelper dh-autoreconf \
    clang equivs fakeroot curl wget lsb-release pkg-config nasm ninja-build mmv \
    bison flex gperf ragel zip unzip python3-venv libtool autogen expect \
    chrpath libmpc-dev libmpfr-dev systemtap-sdt-dev texinfo gettext \
    cmake meson libzstd-dev python3-mako python3-jinja2 python3-packaging python3-yaml
COPY --from=ffmpeg-source /ffmpeg /ffmpeg
COPY --from=sources /usr/local/bin/build-jellyfin /usr/local/bin/
RUN build-jellyfin ffmpeg-deps
RUN build-jellyfin ffmpeg-shaderc
RUN build-jellyfin ffmpeg-graphics
# Keep the fontconfig files installed by the upstream dependency recipe when
# APT installs the distro development libraries during packaging.
COPY packaging/jellyfin/jellyfin-ffmpeg-8.1.3-vfr-vaapi.patch /tmp/vfr-vaapi.patch
RUN git -C /ffmpeg apply /tmp/vfr-vaapi.patch && \
    sed -i '/jellyfin-ffmpeg/ s/+dmd1/+dmd2/g; /Apply the DroidSpaces MOV/a\  * Use the measured frame rate for VAAPI rate control when a VFR input reports its clock frequency.' /usr/local/bin/build-jellyfin && \
    printf '%s\n' 'Dpkg::Options { "--force-confold"; };' > /etc/apt/apt.conf.d/99-jellyfin-build && \
    build-jellyfin ffmpeg

FROM scratch AS server
COPY --from=server-build /packages/ /

FROM scratch AS ffmpeg
COPY --from=ffmpeg-build /packages/ /

FROM scratch AS packages
COPY --from=server / /
COPY --from=ffmpeg / /
