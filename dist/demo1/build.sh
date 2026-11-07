# BMDS Demo #1: an application folder (run by bmds-dist; see dist/README.md).

app="$DESTDIR/apps/demo1.app"
mkdir -p "$app"
"$CC" -O2 -Wall -Wextra -Werror -o "$app/demo1" "$SRC/demo1.c" "$SRC/pal.c" "$SRC/parts/copper.c" "$SRC/parts/plasma.c" \
        "$SRC/parts/stars.c" "$SRC/parts/tunnel.c" "$SRC/parts/fire.c" "$SRC/parts/balls.c" "$SRC/parts/solids.c" \
        "$SRC/song.c"
cp "$SRC/app.desktop" "$app/app.desktop"
