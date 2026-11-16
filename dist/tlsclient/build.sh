# tlsclient: one C file over libtls (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin"
"$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/tlsclient" "$SRC/tlsclient.c"
