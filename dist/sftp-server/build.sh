# sftp-server: two C files (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin"
"$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/sftp-server" "$SRC/main.c" "$SRC/sftp.c"
