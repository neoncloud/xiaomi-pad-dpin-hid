#!/system/bin/sh
# Root-side owner of the Focus Pen Pro DP-in posture request.
MODDIR=${0%/*}
STATE=/dev/.codex-ptp-pen-mode
HAL=vendor.xiaomi.hw.touchfeature.ITouchFeature/default

umask 077
# Android's mksh closes high file descriptors before executing flock.
exec 0<>"$STATE.lock" || exit 1
/system/bin/flock -n 0 || exit 0

set_posture() {
    /system/bin/timeout 2 /system/bin/service call "$HAL" 9 i32 0 i32 20036 i32 "$1" |
        /system/bin/grep -q 'Parcel([[:blank:]]*00000000 00000000'
}

android_posture() {
    local server request
    server=$(/system/bin/pidof system_server)
    if [ -n "$server" ]; then
        request=$(/system/bin/logcat --pid="$server" -d -s MiuiStylusTouchFilmManager:D '*:S' |
            /system/bin/grep 'Focus change to .* stylus posture$' | /system/bin/tail -n 1)
    fi
    case "$request" in
        *', enable stylus posture') echo 1 ;;
        *', disable stylus posture') echo 0 ;;
        *) echo "$baseline" ;;
    esac
}

restore_posture() {
    [ -f "$STATE" ] || return 0
    local target latest
    baseline=$(/system/bin/cat "$STATE")
    case "$baseline" in 0|1) ;; *) return 1 ;; esac
    target=$(android_posture)
    if ! set_posture "$target"; then
        /system/bin/log -t CodexPenMode "restore failed"
        return 1
    fi
    latest=$(android_posture)
    if [ "$latest" != "$target" ] && ! set_posture "$latest"; then
        /system/bin/log -t CodexPenMode "restore after Android focus change failed"
        return 1
    fi
    /system/bin/rm -f "$STATE"
    /system/bin/log -t CodexPenMode "released DP-in posture target=$latest"
}

pid=
pen_route() {
    local comm udc state task
    if [ -z "$pid" ] || [ ! -d "/proc/$pid/task" ]; then
        pid=$(/system/bin/ps -A -o PID,NAME |
            /system/bin/awk '$2=="vendor.xiaomi.usb.dp-service" {print $1; exit}')
    fi
    [ -n "$pid" ] || return 1
    IFS= read -r udc < /config/usb_gadget/g1/UDC || return 1
    case "$udc" in ''|none|*[!a-zA-Z0-9._-]*) return 1 ;; esac
    IFS= read -r state < "/sys/class/udc/$udc/state" || return 1
    case "$state" in configured|suspended) ;; *) return 1 ;; esac
    for task in "/proc/$pid/task/"*/comm; do
        if IFS= read -r comm < "$task" && [ "$comm" = ptp-pen-route ]; then
            return 0
        fi
    done
    return 1
}

trap restore_posture EXIT
trap 'exit 0' TERM HUP INT
baseline=0
attempted=0
restore_failures=0
while [ -f "$MODDIR/module.prop" ] &&
      [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; do
    if pen_route; then
        restore_failures=0
        if [ "$attempted" = 0 ]; then
            attempted=1
            baseline=$(android_posture)
            if [ -f "$STATE" ] || printf '%s\n' "$baseline" > "$STATE"; then
                if set_posture 1; then
                    /system/bin/log -t CodexPenMode "acquired DP-in posture pid=$pid"
                else
                    /system/bin/log -t CodexPenMode "enable failed pid=$pid"
                fi
            fi
        fi
    else
        attempted=0
        if [ -f "$STATE" ] && [ "$restore_failures" -lt 2 ]; then
            restore_posture || restore_failures=$((restore_failures + 1))
        fi
    fi
    /system/bin/sleep 0.3
done
