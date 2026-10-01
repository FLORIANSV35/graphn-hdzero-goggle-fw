#!/bin/sh
# Lists the DVR clips as JSON, the folder of the recordings and its Favorites
# subfolder: [{}, {"n":name,"s":bytes,"d":"Sep 14 00:53","f":1}, ...]  (the first
# element is an empty placeholder; "f":1 marks a favourite). Each folder is
# newest first by modification time, which is when the clip was recorded (the
# Playback conversion keeps the source's timestamps); the page merges them.
# One ls + one awk per folder: forking per file is far too slow on the goggle
# with hundreds of clips. DVR_DIR can be overridden for testing.
DIR="${DVR_DIR:-/mnt/extsd/DCIM/100HDZRO}"

# the clips that have a light copy (Light/NAME.mp4): "l":1
LIGHTS=" $(ls "$DIR/Light" 2>/dev/null | tr '\n' ' ') "

emit() { # folder, favourite flag
    ls -lt "$1" 2>/dev/null | awk -v fav="$2" -v lights="$LIGHTS" '
    NF >= 9 && substr($1, 1, 1) == "-" {
        name = $NF
        if (substr(name, 1, 1) == ".") next
        if (name !~ /\.(ts|mp4)$/) next
        b = name
        sub(/\.[^.]*$/, "", b)
        printf ",{\"n\":\"%s\",\"s\":%s,\"d\":\"%s %s %s\"%s%s}", name, $5, $6, $7, $8, (fav ? ",\"f\":1" : ""), (index(lights, " " b ".mp4 ") ? ",\"l\":1" : "")
    }'
}

printf 'Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n'
printf '[{}'
emit "$DIR" 0
emit "$DIR/Favorites" 1
printf ']\n'
