# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers
# Edit by @physwizz For A127f
## AnyKernel setup
# begin properties
properties() { '
kernel.string=
do.devicecheck=0
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=
device.name2=
device.name3=
device.name4=
device.name5=
supported.versions=
'; } # end properties

# shell variables
block=/dev/block/platform/12100000.dwmmc0/by-name/boot;
bdtbolock=/dev/block/platform/12100000.dwmmc0/by-name/dtb;
is_slot_device=0;
ramdisk_compression=auto;

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

ui_print "- Unpacking boot image";

## AnyKernel install
dump_boot;

mount /system/
mount /system_root/
# Enable Spectrum Support
 ui_print "- Setting Up Spectrum";
 cp /tmp/anykernel/tools/init.spectrum.rc /system_root/init.spectrum.rc;
 chmod 644 /system_root/init.spectrum.rc;
 cp /tmp/anykernel/tools/init.spectrum.sh /system_root/init.spectrum.sh;
  chmod 755 /system_root/init.spectrum.sh;

insert_line /system_root/init.rc "import /init.spectrum.rc" after "import /prism/etc/init/init.rc" "import /init.spectrum.rc";

mount -o rw,remount -t auto /system >/dev/null;
rm -rf /system/bin/vaultkeeper;
rm -rf /system/etc/tima;
rm -rf /system/etc/init/secure_storage_daemon.rc;
rm -rf /system/lib/libvkjni.so;
rm -rf /system/lib/libvkservice.so;
rm -rf /system/lib64/libvkjni.so;
rm -rf /system/lib64/libvkservice.so;
rm -rf /system/priv-app/KLMSAgent;
rm -rf /system/priv-app/KnoxGuard;
rm -rf /system/priv-app/Rlc;
rm -rf /system/priv-app/TeeService;
rm -rf /system/vendor/app/mcRegistry/ffffffffd0000000000000000000000a.tlbin;

# Fix secure storage (Wi-Fi)
cp /tmp/anykernel/tools/libsecure_storage.so /system/vendor/lib/libsecure_storage.so;
chmod 644 /system/vendor/lib/libsecure_storage.so;
cp /tmp/anykernel/tools/libsecure_storage_jni.so /system/vendor/lib/libsecure_storage_jni.so;
chmod 644 /system/vendor/lib/libsecure_storage_jni.so;

# Change permissions
chmod 755 /system/bin/busybox;

# Enable Spectrum Support
 ui_print "- Setting Up Spectrum";
 cp /tmp/anykernel/tools/init.spectrum.rc /system/init.spectrum.rc;
 chmod 644 /system/init.spectrum.rc;
 cp /tmp/anykernel/tools/init.spectrum.sh /system/init.spectrum.sh;
  chmod 755 /system/init.spectrum.sh;

insert_line /system/init.rc "import /init.spectrum.rc" after "import /prism/etc/init/init.rc" "import /init.spectrum.rc";


####################################################################
# Set KNOX to 0x0 on running /system
$RESETPROP ro.boot.warranty_bit "0";
$RESETPROP ro.warranty_bit "0";

# Fix Samsung Related Flags
$RESETPROP ro.fmp_config "1";
$RESETPROP ro.boot.fmp_config "1";

# Fix Samsung Health (CuBz90@XDA)
$RESETPROP ro.config.tima "0";

# Fix safetynet flags
$RESETPROP ro.boot.veritymode "enforcing";
$RESETPROP ro.boot.verifiedbootstate "green";
$RESETPROP ro.boot.flash.locked "1";
$RESETPROP ro.boot.ddrinfo "00000001";

# Google play services wakelock fix (@Tkkg1994)
sleep 1
su -c "pm enable com.google.android.gms/.update.SystemUpdateActivity";
su -c "pm enable com.google.android.gms/.update.SystemUpdateService";
su -c "pm enable com.google.android.gms/.update.SystemUpdateService$ActiveReceiver";
su -c "pm enable com.google.android.gms/.update.SystemUpdateService$Receiver";
su -c "pm enable com.google.android.gms/.update.SystemUpdateService$SecretCodeReceiver";
su -c "pm enable com.google.android.gsf/.update.SystemUpdateActivity";
su -c "pm enable com.google.android.gsf/.update.SystemUpdatePanoActivity";
su -c "pm enable com.google.android.gsf/.update.SystemUpdateService";
su -c "pm enable com.google.android.gsf/.update.SystemUpdateService$Receiver";
su -c "pm enable com.google.android.gsf/.update.SystemUpdateService$SecretCodeReceiver";

# Deepsleep fix (@Chainfire)
for i in `ls /sys/class/scsi_disk/`; do
	cat /sys/class/scsi_disk/$i/write_protect 2>/dev/null | grep 1 >/dev/null
	if [ $? -eq 0 ]; then
		echo 'temporary none' > /sys/class/scsi_disk/$i/cache_type
	fi
done;

umount /system;
umount /system_root;

ui_print "- Installing spectrum";
ui_print "- Installing new boot image";
# Install kernel modules
ui_print "- Installing kernel modules";
mkdir -p /system/lib/modules
cp -r /tmp/anykernel/modules/lib/modules/* /system/lib/modules/
chmod -R 755 /system/lib/modules
chown -R root:root /system/lib/modules

# Install firmware for ath9k_htc
ui_print "- Installing ath9k_htc firmware";
mkdir -p /system/etc/firmware/ath9k_htc
cp -r /tmp/anykernel/modules/etc/firmware/ath9k_htc/* /system/etc/firmware/ath9k_htc/
chmod -R 644 /system/etc/firmware/ath9k_htc
chown -R root:root /system/etc/firmware/ath9k_htc

# Enable ath9k_htc autoload
ui_print "- Enabling ath9k_htc autoload";
mkdir -p /system/etc/modules-load.d
echo "ath9k_htc" > /system/etc/modules-load.d/kali.conf

write_boot;

ui_print "- Done";
ui_print " ";

## end install
