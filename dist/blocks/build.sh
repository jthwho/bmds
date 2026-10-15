# Blocks: an application folder (run by bmds-dist; see dist/README.md).

app="$DESTDIR/apps/blocks.app"
mkdir -p "$app"
"$CC" -O2 -Wall -Wextra -Werror -o "$app/blocks" "$SRC/blocks.c" "$SRC/rules.c"
cp "$SRC/app.desktop" "$app/app.desktop"
python3 "$SRC/assets.py" "$app"
