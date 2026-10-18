# netlat: one C file, and its benchmark's descriptor (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin" "$DESTDIR/share/bench"
"$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/netlat" "$SRC/netlat.c" -lm
cp "$SRC/netlat.bench" "$DESTDIR/share/bench/netlat.bench"
