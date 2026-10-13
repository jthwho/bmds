# The bundle, built with the host's zic by tools/tzbundle (run by bmds-dist;
# see dist/README.md).

mkdir -p "$DESTDIR/share"
python3 "$ROOT/tools/tzbundle" "$SRC" "$DESTDIR/share/zoneinfo.bin"
# IANA's leap-second list as it is (CLOCK_TAI, PTP; devplan/network.md §31e): clock/leapseconds.
cp "$SRC/leap-seconds.list" "$DESTDIR/share/leap-seconds.list"
