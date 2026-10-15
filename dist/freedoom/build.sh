# Freedoom: the two IWADs, into Doom's folder, where it looks after the data folder (run by
# bmds-dist; see dist/README.md).

app="$DESTDIR/apps/doom.app"
mkdir -p "$app"
cp "$SRC/freedoom1.wad" "$SRC/freedoom2.wad" "$app/"
