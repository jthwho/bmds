# netlat: one C file (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin"
"$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/netlat" "$SRC/netlat.c" -lm
