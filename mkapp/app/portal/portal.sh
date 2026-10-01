#!/bin/sh
# Web portal of the WiFi share: DVR list / download / playback / conversion.
#
#   portal.sh start        serve the portal on port 80 (phones open http://<goggle ip>)
#   portal.sh stop         stop the server (a running conversion finishes)
APP=/mnt/app/portal
WWW=$APP/www
ROOT=/tmp/portal
CONV=/tmp/portal_conv
DVR=/mnt/extsd/DCIM/100HDZRO

# The busybox shipped in the services package (known to have the httpd options
# used below); fall back to the system one.
HTTPD="/mnt/app/services/busybox/busybox httpd"
[ -x /mnt/app/services/busybox/busybox ] || HTTPD="httpd"

# The httpd of a previous start runs as "busybox", so killall httpd misses it, and ps
# cuts the command line short ("... httpd -h /tmp/port"): match only the start of it.
stop_servers() {
    pids=$( { ps w 2>/dev/null; ps 2>/dev/null; } | grep -E "httpd -h /tmp/p" | grep -v grep | awk '{print $1}' | sort -u)
    for pid in $pids; do
        kill $pid 2>/dev/null
    done
    killall httpd 2>/dev/null
    sleep 1
    for pid in $pids; do                  # still there: no more asking
        kill -9 $pid 2>/dev/null
    done
}

case "$1" in
start)
    # Light copies (Light/) that nobody watched for 3 weeks go: playing one renews its date (light.cgi).
    # Not while the clock looks unset (before 2025), which would age everything at once or nothing.
    if [ -d $DVR/Light ] && [ "$(date +%s)" -gt 1735689600 ]; then
        find $DVR/Light -name '*.mp4' ! -name '.*' -mtime +21 -exec rm -f {} \;
    fi

    stop_servers
    rm -rf $ROOT
    mkdir -p $ROOT

    ln -s $WWW/index.html $ROOT/index.html
    ln -s $WWW/logo.png $ROOT/logo.png
    ln -s $WWW/cgi-bin $ROOT/cgi-bin
    ln -s $DVR $ROOT/dvr

    cat > $ROOT/httpd.conf <<EOC
.ts:video/mp2t
.m3u8:application/vnd.apple.mpegurl
.mp4:video/mp4
.json:application/json
.js:application/javascript
EOC

    $HTTPD -h $ROOT -c $ROOT/httpd.conf -p 80
    ;;
stop)
    # Only the server stops. A conversion started from the page goes on to its end
    # (the .mp4 is complete, then the .ts is replaced); nothing is left running after that.
    stop_servers
    rm -rf $ROOT
    [ -d $CONV/lock ] || rm -rf $CONV      # keep its progress while a conversion is running
    ;;
esac
