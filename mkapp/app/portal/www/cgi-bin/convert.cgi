#!/bin/sh
# Makes a clip playable in a browser, in place, like the Playback page's Convert to MP4:
#  - a .ts (HEVC in an MPEG-TS, which browsers cannot play) becomes an .mp4 next to it,
#    keeping its recording time, and the .ts is removed once the .mp4 is checked;
#  - an .mp4 made before the hvc1 tag fix (HEVC tagged hev1, refused by Safari and iOS)
#    is rewritten the same way; any other .mp4 is answered "direct" and played as it is.
# Remuxes only (no re-encode), index at the start of the file, one clip at a time.
# Usage: /cgi-bin/convert.cgi?f=NAME.ts|NAME.mp4[&fav=1]
#   -> {"state":"working","percent":N} | {"state":"ready","url":"/dvr/NAME.mp4","converted":1}
#      | {"state":"direct"} | {"state":"busy"} | {"state":"error","why":".."}
# The paths can be overridden for testing off-device.
DIR="${DVR_DIR:-/mnt/extsd/DCIM/100HDZRO}"
STATE="${STATE_DIR:-/tmp/portal_conv}"
BIN="${TS2MP4_BIN:-/mnt/app/app/record/ts2mp4}"
LIBS="${TS2MP4_LIBS:-/lib/libffmpeg:/lib/eyesee-mpp}"

# Three short beeps when a conversion succeeds, like the Playback page's Convert to MP4 (one long
# one when it fails). The buzzer is GPIO 131, driven from the shell as the update scripts do; it
# works whatever the goggle is doing (the share may be over, the goggle back in the video).
BEEP_GPIO="${BEEP_GPIO:-/sys/class/gpio}"
beep_on_off() { # seconds
    echo 1 > "$BEEP_GPIO/gpio131/value" 2>/dev/null
    sleep "$1"
    echo 0 > "$BEEP_GPIO/gpio131/value" 2>/dev/null
}
beep_done() { # 1 = success (three short), 0 = failure (one long)
    [ -d "$BEEP_GPIO/gpio131" ] || echo 131 > "$BEEP_GPIO/export" 2>/dev/null
    echo out > "$BEEP_GPIO/gpio131/direction" 2>/dev/null
    [ -e "$BEEP_GPIO/gpio131/value" ] || return 0
    if [ "$1" -eq 1 ]; then
        beep_on_off 0.1; sleep 0.15; beep_on_off 0.1; sleep 0.15; beep_on_off 0.1
    else
        beep_on_off 0.7
    fi
}

reply() {
    printf 'Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n%s\n' "$1"
    exit 0
}

name=${QUERY_STRING#f=}
name=${name%%&*}
case "$QUERY_STRING" in
*fav=1*) DIR="$DIR/Favorites"; prefix="fav_"; urldir="/dvr/Favorites" ;;
*) prefix=""; urldir="/dvr" ;;
esac
case "$name" in
*[!A-Za-z0-9_.-]* | '' | .*) reply '{"state":"error","why":"bad name"}' ;;
*.ts) kind=ts; base=${name%.ts} ;;
*.mp4) kind=mp4; base=${name%.mp4} ;;
*) reply '{"state":"error","why":"bad name"}' ;;
esac
SRC="$DIR/$name"
OUT="$DIR/$base.mp4"
PART="$DIR/$base.mp4.part"
key="$prefix$name"
mkdir -p "$STATE"

# a conversion is running (this clip or another one)
if [ -d "$STATE/lock" ]; then
    if [ "$(cat "$STATE/lock/name" 2>/dev/null)" = "$key" ]; then
        pct=$(cat "$STATE/lock/progress" 2>/dev/null)
        reply "{\"state\":\"working\",\"percent\":${pct:-0}}"
    fi
    reply '{"state":"busy"}'
fi

# the .ts is gone: it was converted (by this page or by Playback), the .mp4 is there
if [ ! -f "$SRC" ]; then
    [ "$kind" = ts ] && [ -s "$OUT" ] && reply "{\"state\":\"ready\",\"url\":\"$urldir/$base.mp4\",\"converted\":1}"
    reply '{"state":"error","why":"no such clip"}'
fi

# finished with a failure last time: report it once, then allow a retry
if [ -f "$STATE/failed_$key" ]; then
    rm -f "$STATE/failed_$key"
    reply '{"state":"error","why":"conversion failed"}'
fi

if [ "$kind" = ts ]; then
    # both there: the .ts could not be removed last time, the .mp4 is the good one
    [ -s "$OUT" ] && reply "{\"state\":\"ready\",\"url\":\"$urldir/$base.mp4\",\"converted\":1}"
else
    # an .mp4 only needs work when its HEVC is tagged hev1 (index at the start or the end)
    found=$( { head -c 3000000 "$SRC"; tail -c 3000000 "$SRC"; } 2>/dev/null | grep -c hev1)
    [ "${found:-0}" -gt 0 ] || reply '{"state":"direct"}'
fi

mkdir "$STATE/lock" 2>/dev/null || reply '{"state":"busy"}'
echo "$key" > "$STATE/lock/name"
echo 0 > "$STATE/lock/progress"

( export LD_LIBRARY_PATH="$LIBS"
  TS2MP4_FASTSTART=1 "$BIN" "$SRC" "$PART" "$STATE/lock/progress" > "$STATE/last.log" 2>&1
  code=$?
  # the first method failed (not enough room for the index): try the other one
  if [ $code -ne 0 ]; then
      TS2MP4_FASTSTART=2 "$BIN" "$SRC" "$PART" "$STATE/lock/progress" >> "$STATE/last.log" 2>&1
      code=$?
  fi
  ok=0
  if [ $code -eq 0 ] && [ -s "$PART" ]; then
      # a remux is about the size of its source: a much smaller result is a truncated one
      srcsz=$(wc -c < "$SRC"); outsz=$(wc -c < "$PART")
      [ $((outsz * 100)) -ge $((srcsz * 90)) ] && ok=1
  fi
  if [ $ok -eq 1 ]; then
      mv -f "$PART" "$OUT"                 # ts2mp4 gave it the source's date
      [ "$kind" = ts ] && rm -f "$SRC"     # only after the .mp4 is in place
  else
      rm -f "$PART"
      touch "$STATE/failed_$key"
  fi
  rm -rf "$STATE/lock"
  beep_done $ok
) < /dev/null > /dev/null 2>&1 &

reply '{"state":"working","percent":0}'
