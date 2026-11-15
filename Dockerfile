# Builds czduke for the CardputerZero (Debian trixie, arm64) on any Docker host.
#
#   docker build --output type=local,dest=dist .                  -> dist/czduke_<ver>_arm64.deb
#   docker build --target smoke-out --output type=local,dest=out . -> test screenshots
#   docker build --target smoke-arm64-out --output type=local,dest=out-arm64 .
#                                                 -> the same test on the arm64 binary
#
# On an arm64 host the build is native; on x86 it cross-compiles, so no QEMU
# emulation is needed either way.

FROM debian:trixie AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    apt-get update; \
    if [ "$(dpkg --print-architecture)" = arm64 ]; then \
        apt-get install -y --no-install-recommends build-essential pkg-config git ca-certificates \
            libsdl2-dev; \
        printf 'CROSS_COMPILE=\nPKG_CONFIG=pkg-config\n' >/etc/czduke-toolchain; \
    else \
        dpkg --add-architecture arm64; \
        apt-get update; \
        apt-get install -y --no-install-recommends build-essential crossbuild-essential-arm64 \
            pkg-config git ca-certificates libsdl2-dev:arm64; \
        printf 'CROSS_COMPILE=aarch64-linux-gnu-\nPKG_CONFIG=aarch64-linux-gnu-pkg-config\n' >/etc/czduke-toolchain; \
    fi; \
    rm -rf /var/lib/apt/lists/*

FROM toolchain AS build
WORKDIR /src
COPY Makefile LICENSE BUILDLIC.TXT ./
COPY src ./src
COPY packaging ./packaging
RUN set -eux; . /etc/czduke-toolchain; \
    make -j"$(nproc)" deb CROSS_COMPILE="$CROSS_COMPILE" PKG_CONFIG="$PKG_CONFIG"; \
    dpkg-deb --info dist/*.deb; dpkg-deb --contents dist/*.deb

# The shareware episode, for the test only. It is downloaded here, used by the
# smoke stage and never ends up in the package or the repository.
FROM debian:trixie AS shareware
RUN apt-get update && apt-get install -y --no-install-recommends python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY packaging/get-shareware /usr/local/bin/get-shareware
RUN get-shareware /grp

# Runs the game for a few seconds against a fake 320x170 RGB565 framebuffer
# and scripted key events, using the build host's own architecture.
FROM debian:trixie AS smoke-build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config git \
        ca-certificates libsdl2-dev libfluidsynth3 timgm6mb-soundfont python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile LICENSE BUILDLIC.TXT ./
COPY src ./src
COPY packaging ./packaging
RUN make -j"$(nproc)" deb STRIP=strip && dpkg-deb -x dist/*.deb /
COPY --from=shareware /grp/duke3d.grp /grp/duke3d.grp
COPY tests ./tests

FROM smoke-build AS smoke
RUN tests/smoke.sh build/czduke /grp /out

FROM scratch AS smoke-out
COPY --from=smoke /out/ /

# The same test against the arm64 binary that goes into the package. On an
# x86 host it runs under QEMU user-mode emulation (no binfmt setup needed).
FROM build AS smoke-arm64
RUN set -eux; apt-get update; \
    apt-get install -y --no-install-recommends python3 \
        $([ "$(dpkg --print-architecture)" = arm64 ] || echo qemu-user); \
    rm -rf /var/lib/apt/lists/*
COPY --from=shareware /grp/duke3d.grp /grp/duke3d.grp
COPY tests ./tests
RUN set -eux; BIN=build/czduke; \
    if [ "$(dpkg --print-architecture)" != arm64 ]; then \
        printf '#!/bin/sh\nexec qemu-aarch64 /src/build/czduke "$@"\n' >/usr/local/bin/czduke-arm64; \
        chmod +x /usr/local/bin/czduke-arm64; BIN=/usr/local/bin/czduke-arm64; \
    fi; \
    tests/smoke.sh "$BIN" /grp /out

FROM scratch AS smoke-arm64-out
COPY --from=smoke-arm64 /out/ /

FROM scratch
COPY --from=build /src/dist/ /
