# syntax=docker/dockerfile:1

# Builds the ZERODROID NRO without requiring devkitPro, Docker, or any Switch
# libraries on the host.
#
#   DOCKER_BUILDKIT=1 docker build --output type=local,dest=dist .
#
# The resulting NRO is written to dist/ZERODROID_v<version>.nro.

FROM devkitpro/devkita64:latest AS build

ENV DEBIAN_FRONTEND=noninteractive
ENV DEVKITPRO=/opt/devkitpro
ENV PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH

# Host-side tools needed to fetch the WebRTC dependency sources.
RUN apt-get update \
    && apt-get install -y --no-install-recommends git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Switch toolchain, build tools, shader compiler and portlibs required by
# switch/Makefile (SDL2, libcurl, FFmpeg/libavcodec, Opus, EGL/Mesa, deko3d).
RUN dkp-pacman -Sy --noconfirm \
    && dkp-pacman -S --needed --noconfirm \
        devkitA64 \
        devkit-env \
        general-tools \
        libnx \
        deko3d \
        switch-tools \
        uam \
        switch-pkg-config \
        switch-sdl2 \
        switch-sdl2_ttf \
        switch-sdl2_image \
        switch-curl \
        switch-ffmpeg \
        switch-libopus \
        switch-mesa \
        switch-libdrm_nouveau \
    && dkp-pacman -Scc --noconfirm

# The libpeer/mbedtls/usrsctp/libsrtp stack is not packaged by devkitPro.
# Build it into /work/deps/switch using the repo's existing dependency script
# in "no nested Docker" mode.
WORKDIR /work
COPY switch/deps /work/deps
RUN bash /work/deps/build-switch.sh --container

# Build the application. The deps live outside the build context so they can be
# copied in after the source tree without being hidden by the bind mount.
WORKDIR /build
COPY . /build
RUN cp -a /work/deps/switch /build/switch/deps/switch

WORKDIR /build/switch
RUN make -j"$(nproc)"

# Artifact-only stage: `docker build --output ... .` extracts this directory.
FROM scratch AS artifact
COPY --from=build /build/switch/*.nro /
