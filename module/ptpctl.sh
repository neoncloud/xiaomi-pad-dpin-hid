#!/system/bin/sh
case "${1:-status}" in
  start) /system/bin/setprop debug.codex.ptp.control CodexPTPModule:run ;;
  stop) /system/bin/setprop debug.codex.ptp.control CodexPTPModule:stop ;;
  palm-test)
    threshold=${2:-21}
    case "$threshold" in
      21|22) /system/bin/setprop debug.codex.ptp.palm_test "$threshold" ;;
      *) echo "palm-test supports 21 or 22"; exit 2 ;;
    esac
    ;;
  palm-test-off) /system/bin/setprop debug.codex.ptp.palm_test off ;;
  status)
    echo "boot=$(/system/bin/cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
    echo "selinux=$(/system/bin/getenforce 2>/dev/null)"
    echo "service_pid=$(/system/bin/pidof vendor.xiaomi.usb.dp-service 2>/dev/null)"
    echo "control=$(/system/bin/getprop debug.codex.ptp.control)"
    palm=$(/system/bin/getprop debug.codex.ptp.palm_test)
    echo "palm_test=${palm:-21}"
    echo "fds:"
    pid=$(/system/bin/pidof vendor.xiaomi.usb.dp-service 2>/dev/null)
    [ -n "$pid" ] && /system/bin/ls -l "/proc/$pid/fd" | /system/bin/grep -E 'hidg0|hidg1|event' || true
    echo "events:"
    /system/bin/logcat -d -t 80 -s CodexPtpNative:I '*:S' 2>/dev/null | /system/bin/tail -n 20
    ;;
  *) echo "usage: $0 {start|stop|status|palm-test|palm-test-off}"; exit 2 ;;
esac
