# Blocks: an application folder (run by bmds-dist; see dist/README.md), its icons built into the
# program (sdk/tools/bmds-icon-notes).

app="$DESTDIR/apps/blocks.app"
mkdir -p "$app"
python3 "$SRC/assets.py" --icons "$BUILD"
python3 "$ROOT/sdk/tools/bmds-icon-notes" -o "$BUILD/icons.S" blocks "$BUILD/icon-16.png" "$BUILD/icon-32.png"
"$CC" -O2 -Wall -Wextra -Werror -o "$app/blocks" "$SRC/blocks.c" "$SRC/rules.c" "$BUILD/icons.S"
cp "$SRC/app.desktop" "$app/app.desktop"
cp "$SRC/controls.conf" "$app/controls.conf"
python3 "$SRC/assets.py" "$app"
