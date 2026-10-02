#!/bin/bash
# Run a click the way an Ubuntu Touch phone does, in the image from image.sh.
#
#   device.sh start <arm64|amd64> [wayland-N]   start the device container
#   device.sh install <file.click>              click install, phone hook loads the profile
#   device.sh launch <log> [pkg] [app]          aa-exec-click, desktop Exec, cwd = click dir
#   device.sh stop-app                          stop the app (SIGTERM, as closing it does)
#   device.sh rm                                remove the container
#
# With a wayland-N argument the app draws into that host Wayland socket, such
# as the isolated headless gnome-shell from headless-shell.sh. Without one,
# Mir runs inside the container on a virtual 2340x1080 output.
#
# Why privileged: click install loads the AppArmor profile into the kernel,
# and only a privileged container gets the plain profile label a phone gives
# the app. Unprivileged, a desktop kernel turns aa-exec into a profile stack
# (unconfined//&profile), which denies unix-socket traffic a phone allows.
# Inside the container /dev/input is masked and the display card removed, so
# nothing reaches the host's input devices or screen.
set -euo pipefail

NAME=ut-device
cmd="${1:?usage: device.sh start|install|launch|stop-app|rm ...}"
shift

in_container() { docker exec "$NAME" bash -c "$1"; }

case "$cmd" in
start)
    arch="${1:?usage: device.sh start arm64|amd64 [wayland-N]}"
    socket="${2:-}"
    mount=()
    if [ -n "$socket" ]; then
        # The profile allows owner /run/user/*/wayland-[0-9]*: keep the name.
        mount=(-v "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$socket:/run/user/1000/$socket")
    fi
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    docker run -d --platform "linux/$arch" --name "$NAME" --privileged \
        -v /sys/kernel/security:/sys/kernel/security "${mount[@]}" \
        "ut-device-$arch" sleep infinity >/dev/null
    in_container '
        mount -t tmpfs none /dev/input
        rm -f /dev/dri/card*
        # What a Mesa device port ships in graphics.d; Halium ports list
        # their kgsl/mali nodes there instead. Without it a confined app
        # cannot open the render node and Mesa falls back to llvmpipe.
        mkdir -p /usr/share/apparmor/hardware/graphics.d
        printf "%s\n" "/dev/dri/ r," "/dev/dri/renderD* rw," "/sys/dev/char/226:* r," "/sys/devices/** r," \
            > /usr/share/apparmor/hardware/graphics.d/mesa-dri
        if [ -e /dev/dri/renderD128 ]; then
            g=$(stat -c %g /dev/dri/renderD128)
            getent group "$g" >/dev/null || groupadd -g "$g" render
            usermod -aG "$g" phablet
        fi'
    if [ -z "$socket" ]; then
        in_container 'su -s /bin/bash phablet -c "XDG_RUNTIME_DIR=/run/user/1000 setsid nohup miral-shell \
            --platform-display-libs mir:virtual --virtual-output 2340x1080 \
            --add-wayland-extensions zwlr_screencopy_manager_v1 > /tmp/mir.log 2>&1 &"
            for i in $(seq 120); do [ -S /run/user/1000/wayland-0 ] && break; sleep 1; done'
        echo "$NAME up ($arch, Mir on wayland-0)"
    else
        echo "$NAME up ($arch, drawing into $socket)"
    fi
    ;;
install)
    click="${1:?usage: device.sh install <file.click>}"
    docker cp "$click" "$NAME:/tmp/app.click"
    in_container 'click install --user=phablet --allow-unauthenticated /tmp/app.click 2>&1 \
            | grep -v -E "debsig|does not exist in any database" || true
        # Reload so the graphics.d rules written after the hook ran are in.
        for p in /var/lib/apparmor/profiles/click_*; do apparmor_parser -r -W "$p"; done
        grep -E "^[a-z0-9.]+_[a-z0-9]+_[0-9.]+ " /sys/kernel/security/apparmor/profiles | grep -v "^unconfined"'
    ;;
launch)
    log="${1:?usage: device.sh launch <log> [pkg] [app]}"
    pkg="${2:-xonotictouch.dixonsolutions}"
    app="${3:-xonotic}"
    in_container "
        P=/opt/click.ubuntu.com/$pkg/current
        ver=\$(readlink \$P)
        sock=\$(cd /run/user/1000 && ls -d wayland-[0-9]* 2>/dev/null | grep -v lock | head -1)
        cd \$P
        # CAP_MAC_ADMIN/SYS_ADMIN/MAC_OVERRIDE only let aa-exec switch profile
        # outright, as a phone does; the profile grants no capability, so the
        # app cannot use them. XDG_CURRENT_DESKTOP=Lomiri is what a Lomiri
        # session gives the apps it launches.
        setpriv --reuid=phablet --regid=phablet --init-groups \
            --inh-caps=+mac_admin,+sys_admin,+mac_override \
            --ambient-caps=+mac_admin,+sys_admin,+mac_override -- \
            env -i HOME=/home/phablet USER=phablet PATH=/usr/local/bin:/usr/bin:/bin \
                XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=\$sock XDG_SESSION_TYPE=wayland \
                XDG_CURRENT_DESKTOP=Lomiri APP_DIR=\$P \
            setsid nohup aa-exec-click -p ${pkg}_${app}_\$ver -- /bin/sh -c 'exec bin/start.sh' \
            > $log 2>&1 &
        echo launched ${pkg}_${app}_\$ver"
    ;;
stop-app)
    in_container 'pgrep -f "^/opt/click" | xargs -r kill; sleep 3
        pgrep -f "^/bin/sh bin/start.sh" | xargs -r kill -9; true'
    ;;
rm)
    docker rm -f "$NAME" >/dev/null && echo "removed $NAME"
    ;;
*)
    echo "device.sh: unknown command $cmd" >&2
    exit 2
    ;;
esac
