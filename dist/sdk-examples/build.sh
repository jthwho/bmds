# The SDK's audio examples, each one C file (run by bmds-dist; see dist/README.md).

mkdir -p "$DESTDIR/bin"
for program in tone player keytone midimon; do
        "$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/bin/$program" "$SRC/$program/$program.c"
done
# Application folders (devplan/apps.md §3): the program and its app.desktop, in apps/NAME.app.
for app in surfdemo appinfo; do
        mkdir -p "$DESTDIR/apps/$app.app"
        "$CC" -O2 -Wall -Wextra -Werror -o "$DESTDIR/apps/$app.app/$app" "$SRC/$app/$app.c"
        cp "$SRC/$app/app.desktop" "$DESTDIR/apps/$app.app/app.desktop"
done
# Launcher entries for the panel (devplan/gui.md G11).
mkdir -p "$DESTDIR/share/applications"
cp "$PKGDIR"/applications/*.desktop "$DESTDIR/share/applications/"
