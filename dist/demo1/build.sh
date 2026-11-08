# BMDS Demo #1: an application folder (run by bmds-dist; see dist/README.md).

app="$DESTDIR/apps/demo1.app"
mkdir -p "$app"
# Its icons, built into the program (devplan/gui.md §4j).
python3 "$SRC/icon.py" .
python3 "$ROOT/sdk/tools/bmds-icon-notes" -o icons.S demo1 icon-16.png icon-32.png
"$CC" -O2 -Wall -Wextra -Werror -o "$app/demo1" "$SRC/demo1.c" "$SRC/pal.c" "$SRC/parts/copper.c" "$SRC/parts/plasma.c" \
        "$SRC/parts/stars.c" "$SRC/parts/tunnel.c" "$SRC/parts/fire.c" "$SRC/parts/balls.c" "$SRC/parts/solids.c" \
        "$SRC/parts/roto.c" "$SRC/parts/boot.c" "$SRC/parts/credits.c" "$SRC/voice.c" "$SRC/song.c" icons.S
cp "$SRC/app.desktop" "$app/app.desktop"
