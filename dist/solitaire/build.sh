# Solitaire: an application folder (run by bmds-dist; see dist/README.md), its icons built into the
# program (sdk/tools/bmds-icon-notes).

app="$DESTDIR/apps/solitaire.app"
mkdir -p "$app"
python3 "$SRC/assets.py" --icons "$BUILD"
python3 "$ROOT/sdk/tools/bmds-icon-notes" -o "$BUILD/icons.S" solitaire "$BUILD/icon-16.png" "$BUILD/icon-32.png"
"$CC" -O2 -Wall -Wextra -Werror -o "$app/solitaire" "$SRC/solitaire.c" "$SRC/rules.c" "$BUILD/icons.S"
cp "$SRC/app.desktop" "$app/app.desktop"
python3 "$SRC/assets.py" "$app"
