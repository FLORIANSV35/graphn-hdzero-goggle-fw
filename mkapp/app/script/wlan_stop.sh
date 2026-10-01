#!/bin/sh
/mnt/app/app/record/gogglecmd -live quit
sleep 1
killall rtspLive
/mnt/app/portal/portal.sh stop
killall hostapd
killall udhcpd
killall dropbear
ifconfig wlan0 down
rmmod xradio_wlan.ko
rmmod xradio_core.ko
rmmod xradio_mac.ko
