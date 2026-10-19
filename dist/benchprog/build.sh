# benchprog: one C file, its benchmarks' descriptor and the MIDI file one plays (run by bmds-dist;
# see dist/README.md).

mkdir -p "$DESTDIR/bin" "$DESTDIR/share/bench"
"$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/benchprog" "$SRC/benchprog.c"
# The same, flagged as drawing to the screen (BMDS_PROGRAM_DISPLAY): surface.present runs where the
# kernel starts a game, away from the display's core (devplan/bench.md §17).
BMDS_DISPLAY=1 "$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/benchprog-display" "$SRC/benchprog.c"
cp "$SRC/benchprog.bench" "$DESTDIR/share/bench/benchprog.bench"
cp "$ROOT/tests/host/synth/scale.mid" "$DESTDIR/share/bench/scale.mid"
