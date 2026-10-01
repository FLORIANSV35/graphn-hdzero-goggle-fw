#!/bin/sh
/mnt/app/app/record/gogglecmd -live quit
sleep 1
killall rtspLive
/mnt/app/portal/portal.sh stop
killall hostapd
killall udhcpd
killall wpa_supplicant
killall udhcpc
sleep 1
# The bundled DHCP server can remain blocked after SIGTERM.
killall -9 udhcpd 2>/dev/null
killall dropbear
ifconfig wlan0 down
rmmod xradio_wlan.ko
rmmod xradio_core.ko
rmmod xradio_mac.ko
