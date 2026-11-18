#!/system/bin/sh
ui_print "Native USB Precision Touchpad"
set_perm "$MODPATH/system/odm/bin/vendor.xiaomi.usb.dp-service" 0 0 0755
set_perm "$MODPATH/system/odm/bin/vendor.xiaomi.usb.dp-service.real" 0 0 0755
set_perm "$MODPATH/system/odm/lib64/libptp_auto.so" 0 0 0644
set_perm "$MODPATH/system/odm/lib64/libshadowhook.so" 0 0 0644
set_perm "$MODPATH/system/odm/lib64/libshadowhook_nothing.so" 0 0 0644
chcon u:object_r:hal_usb_dp_relay_default_exec:s0   "$MODPATH/system/odm/bin/vendor.xiaomi.usb.dp-service" || abort "wrapper context failed"
chcon u:object_r:vendor_file:s0   "$MODPATH/system/odm/bin/vendor.xiaomi.usb.dp-service.real" || abort "real context failed"
for f in libptp_auto.so libshadowhook.so libshadowhook_nothing.so; do
  chcon u:object_r:vendor_file:s0 "$MODPATH/system/odm/lib64/$f" || abort "$f context failed"
done
