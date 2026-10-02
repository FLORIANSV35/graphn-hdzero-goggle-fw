#!/bin/sh
# Light copy of a clip, made by the goggle's video hardware (src/tsproxy/main.c): the clip is decoded
# and re-encoded at no more than 30 images per second and about 8 Mbit/s, the audio is copied, and the
# result goes in the Light/ folder next to the clips (Playback ignores it). The original is not touched.
# It is for watching over the WiFi, whose speed (about 18 Mbit/s) a DVR clip (22 to 35 Mbit/s) exceeds.
# Usage: /cgi-bin/light.cgi?f=NAME.ts|NAME.mp4[&fav=1][&auto=1]
#   auto=1: the clip's frame rate is read first; up to 30 images/s -> {"state":"fast"} and nothing is made
#   -> {"state":"fast","fps":N} | {"state":"working","percent":N} | {"state":"ready","url":"/dvr/Light/NAME.mp4"}
#      | {"state":"busy"} | {"state":"error","why":".."}
# A light copy nobody watched for 3 weeks is removed (portal.sh start); each time it is played its date is renewed.
# One job at a time with the conversions (same lock). The paths can be overridden for testing.
DIR="${DVR_DIR:-/mnt/extsd/DCIM/100HDZRO}"
STATE="${STATE_DIR:-/tmp/portal_conv}"
BIN="${TSPROXY_BIN:-/mnt/app/app/record/tsproxy}"
LIBS="${TS2MP4_LIBS:-/lib/libffmpeg:/lib/eyesee-mpp}"
BEEP_GPIO="${BEEP_GPIO:-/sys/class/gpio}"
KBPS=8000

beep_on_off() { echo 1 > "$BEEP_GPIO/gpio131/value" 2>/dev/null; sleep "$1"; echo 0 > "$BEEP_GPIO/gpio131/value" 2>/dev/null; }
beep_done() { # 1 = three short, 0 = one long
    [ -d "$BEEP_GPIO/gpio131" ] || echo 131 > "$BEEP_GPIO/export" 2>/dev/null
    echo out > "$BEEP_GPIO/gpio131/direction" 2>/dev/null
    [ -e "$BEEP_GPIO/gpio131/value" ] || return 0
    if [ "$1" -eq 1 ]; then beep_on_off 0.1; sleep 0.15; beep_on_off 0.1; sleep 0.15; beep_on_off 0.1; else beep_on_off 0.7; fi
}
reply() { printf 'Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n%s\n' "$1"; exit 0; }

name=${QUERY_STRING#f=}
name=${name%%&*}
case "$QUERY_STRING" in
*fav=1*) SRCDIR="$DIR/Favorites"; prefix="fav_" ;;
*) SRCDIR="$DIR"; prefix="" ;;
esac
case "$name" in
*[!A-Za-z0-9_.-]* | '' | .*) reply '{"state":"error","why":"bad name"}' ;;
*.ts) base=${name%.ts} ;;
*.mp4) base=${name%.mp4} ;;
*) reply '{"state":"error","why":"bad name"}' ;;
esac
SRC="$SRCDIR/$name"
LIGHT="$DIR/Light"                    # one folder for all the light copies, favourites too
OUT="$LIGHT/$base.mp4"
PART="$LIGHT/.$base.mp4"              # hidden while being written; the name must end in .mp4
key="light_$prefix$name"
mkdir -p "$STATE"

if [ -d "$STATE/lock" ]; then
    if [ "$(cat "$STATE/lock/name" 2>/dev/null)" = "$key" ]; then
        pct=$(cat "$STATE/lock/progress" 2>/dev/null)
        reply "{\"state\":\"working\",\"percent\":${pct:-0},\"info\":\"$(cat "$STATE/lock/info" 2>/dev/null)\"}"
    fi
    reply '{"state":"busy"}'
fi
if [ -s "$OUT" ]; then
    touch "$OUT"                      # watched: the date of the copy is the date it was last watched (portal.sh start removes the ones not watched for 3 weeks)
    reply "{\"state\":\"ready\",\"url\":\"/dvr/Light/$base.mp4\"}"
fi
[ -f "$SRC" ] || reply '{"state":"error","why":"no such clip"}'
[ -x "$BIN" ] || reply '{"state":"error","why":"tsproxy is not installed"}'
if [ -f "$STATE/failed_$key" ]; then
    rm -f "$STATE/failed_$key"
    reply '{"state":"error","why":"light copy failed"}'
fi

# automatic mode: a clip of 30 images/s or less plays as it is
case "$QUERY_STRING" in
*auto=1*)
    probe=$(LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$LIBS" "$BIN" --probe "$SRC" 2>/dev/null)
    fps=$(echo "$probe" | sed -n 's/^fps=//p')
    fps=${fps:-0}
    decl=$(echo "$probe" | sed -n 's/^declared=//p')
    meas=$(echo "$probe" | sed -n 's/^measured=//p')
    info="detected ${fps} images/s (container ${decl:-0}, measured ${meas:-0})"
    if [ "$fps" -gt 0 ] && [ "$fps" -le 30 ]; then reply "{\"state\":\"fast\",\"fps\":$fps,\"info\":\"$info\"}"; fi
    ;;
esac

mkdir "$STATE/lock" 2>/dev/null || reply '{"state":"busy"}'
echo "$key" > "$STATE/lock/name"
echo 0 > "$STATE/lock/progress"
echo "${info:-}" > "$STATE/lock/info"
mkdir -p "$LIGHT"

( export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$LIBS"
  rm -f "$PART"
  "$BIN" "$SRC" "$PART" $KBPS 0 "$STATE/light.log" 300 "$STATE/lock/progress" > "$STATE/light.out" 2>&1
  code=$?
  if [ $code -ne 0 ]; then                     # the index did not fit in the room kept for it: the other method
      rm -f "$PART"
      TSPROXY_FASTSTART=2 "$BIN" "$SRC" "$PART" $KBPS 0 "$STATE/light.log" 300 "$STATE/lock/progress" > "$STATE/light.out" 2>&1
      code=$?
  fi
  ok=0
  if [ $code -eq 0 ] && [ -s "$PART" ] && [ "$(wc -c < "$PART")" -gt 100000 ]; then ok=1; fi
  if [ $ok -eq 1 ]; then
      mv -f "$PART" "$OUT"
  else
      rm -f "$PART"
      touch "$STATE/failed_$key"
  fi
  rm -rf "$STATE/lock"
  beep_done $ok
) < /dev/null > /dev/null 2>&1 &

reply "{\"state\":\"working\",\"percent\":0,\"info\":\"${info:-}\"}"
