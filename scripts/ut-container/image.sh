#!/bin/bash
# Build an Ubuntu Touch 24.04 userland image to run a click under its real
# AppArmor profile, without a phone (docs/UBUNTU_TOUCH_LAUNCH.md, section 5).
#
#   scripts/ut-container/image.sh arm64   # release clicks; needs arm64 binfmt
#   scripts/ut-container/image.sh amd64   # the test-only amd64 CI click, native speed
#
# The image has the UBports repository, click and click-apparmor (the phone's
# own install hook and profile generator), the libraries the game takes from
# the phone (SDL2, libjpeg, zlib, libpng, libcurl), Mesa, and Mir. The UBports
# key comes from the clickable CI image, which carries it.
#
# arm64 on an x86 PC needs emulation first, until the next reboot:
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
set -euo pipefail

ARCH="${1:?usage: image.sh arm64|amd64}"
case "$ARCH" in
    arm64|amd64) ;;
    *) echo "image.sh: arch must be arm64 or amd64" >&2; exit 2 ;;
esac
IMAGE="ut-device-$ARCH"
CFG="$(mktemp -d)"
trap 'rm -rf "$CFG"' EXIT

docker run --rm -v "$CFG:/o" clickable/ci-ut24.04-1.x-amd64 sh -c '
    cp /etc/apt/sources.list.d/ubports.list /etc/apt/trusted.gpg.d/ubports.gpg /o/
    cp /etc/apt/preferences.d/* /o/ 2>/dev/null || true
    chown -R '"$(id -u):$(id -g)"' /o'

docker pull -q --platform "linux/$ARCH" ubuntu:24.04 >/dev/null
docker rm -f "$IMAGE-setup" >/dev/null 2>&1 || true
# Plain docker run + commit: the legacy builder cannot export a foreign-arch
# layer, and buildx is not always installed.
docker run --platform "linux/$ARCH" --name "$IMAGE-setup" -v "$CFG:/cfg:ro" ubuntu:24.04 bash -c '
    set -e
    cp /cfg/ubports.gpg /etc/apt/trusted.gpg.d/
    cp /cfg/ubports.list /etc/apt/sources.list.d/
    for f in /cfg/*.pref; do [ -f "$f" ] && cp "$f" /etc/apt/preferences.d/; done
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        click click-apparmor apparmor apparmor-utils apparmor-easyprof-ubuntu \
        libsdl2-2.0-0 libjpeg8 zlib1g libpng16-16t64 libcurl4t64 \
        libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri libwayland-egl1 \
        mir-demos mir-platform-graphics-virtual mir-platform-rendering-egl-generic \
        mir-platform-input-evdev10 dmz-cursor-theme grim \
        procps psmisc util-linux ca-certificates >/dev/null
    # What ubuntu-sdk-libs installs on every phone; the whole Qt stack is not
    # needed to install a click that only declares the framework.
    mkdir -p /usr/share/click/frameworks
    printf "Base-Name: ubuntu-sdk\nBase-Version: 24.04-1.x\n" \
        > /usr/share/click/frameworks/ubuntu-touch-24.04-1.x.framework
    # phablet as uid 1000, so it can use a Wayland socket owned by the host user.
    if id ubuntu >/dev/null 2>&1; then userdel -r ubuntu 2>/dev/null || true; fi
    if id phablet >/dev/null 2>&1; then usermod -u 1000 phablet; groupmod -g 1000 phablet || true
    else groupadd -g 1000 phablet; useradd -m -u 1000 -g 1000 -s /bin/bash phablet; fi
    chown -R phablet:phablet /home/phablet
    # A phone has these; their absence makes a confined mkdir of the parent fail.
    su -s /bin/sh phablet -c "mkdir -p ~/.config ~/.cache ~/.local/share"
    mkdir -p /run/user/1000 && chown phablet:phablet /run/user/1000 && chmod 700 /run/user/1000
    rm -rf /var/lib/apt/lists/*'
docker commit "$IMAGE-setup" "$IMAGE" >/dev/null
docker rm "$IMAGE-setup" >/dev/null
echo "built $IMAGE"
