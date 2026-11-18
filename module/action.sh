#!/system/bin/sh
/system/bin/setprop debug.codex.ptp.control CodexPTPModule:run
/system/bin/nohup /system/bin/sh "${0%/*}/pen-mode.sh" </dev/null >/dev/null 2>&1 &
echo "native PTP armed"
