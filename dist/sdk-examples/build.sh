# The SDK's audio examples, each one C file (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin"
for program in tone player keytone midimon; do
        "$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/$program" "$SRC/$program/$program.c"
done
# Launcher entries for the panel (devplan/gui.md G11).
mkdir -p "$DESTDIR/share/applications"
cp "$PKGDIR"/applications/*.desktop "$DESTDIR/share/applications/"
