# The bundle, built with the host's zic by tools/tzbundle (run by bmds-dist;
# see dist/README.md).

mkdir -p "$DESTDIR/share"
python3 "$ROOT/tools/tzbundle" "$SRC" "$DESTDIR/share/zoneinfo.bin"
