#!/bin/sh
set -eu
if [ "$(id -u)" != 0 ]; then
    exec sudo /usr/local/bin/m4-accelerated-hyprland "$@"
fi
case "${1:-start}" in
    start)
        exec systemd-run --unit=m4-accelerated-hyprland --collect \
            /root/hyprland-validation/start-omarchy.sh "${SUDO_USER:-e}" 3600
        ;;
    stop)
        touch /run/m4-hyprland-stop
        ;;
    status)
        systemctl status m4-accelerated-hyprland --no-pager
        ;;
    *)
        echo 'Usage: m4-accelerated-hyprland [start|stop|status]' >&2
        exit 2
        ;;
esac
