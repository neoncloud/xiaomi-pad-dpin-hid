#!/system/bin/sh
MODDIR=${0%/*}
chmod 0755 "$MODDIR/system/odm/bin/vendor.xiaomi.usb.dp-service"   "$MODDIR/system/odm/bin/vendor.xiaomi.usb.dp-service.real" 2>/dev/null || true
chcon u:object_r:hal_usb_dp_relay_default_exec:s0   "$MODDIR/system/odm/bin/vendor.xiaomi.usb.dp-service" 2>/dev/null || true
chcon u:object_r:vendor_file:s0   "$MODDIR/system/odm/bin/vendor.xiaomi.usb.dp-service.real" 2>/dev/null || true
