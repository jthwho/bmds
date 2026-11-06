# preso: the core, its scripts, the BMDS platform, md4c (thirdparty/md4c), AngelScript
# (thirdparty/angelscript) and libangel (programs/angel), run by bmds-dist (dist/README.md).

AS="$ROOT/thirdparty/angelscript"
cp "$PKGDIR/Makefile" Makefile
$BMDS_MAKE MD4C="$ROOT/thirdparty/md4c" AS="$AS" ANGEL="$ROOT/programs/angel" SRC="$SRC" ROOT="$ROOT" CC="$CC" \
        CXX="$CXX" VERSION="$VERSION" preso
mkdir -p "$DESTDIR/bin" "$DESTDIR/share/licenses/preso"
cp preso "$DESTDIR/bin/preso"
cp "$AS/LICENSE" "$DESTDIR/share/licenses/preso/angelscript.LICENSE"
# The example decks (devplan/preso.md §8a, §8e): Preso's tour, the BMDS talk and the scenes'
# showcase, with their scripts.
mkdir -p "$DESTDIR/share/preso"
cp -r "$SRC/examples" "$DESTDIR/share/preso/examples"
# The launcher's entry (devplan/preso.md §8b): the start window, or a deck Files opens with it.
mkdir -p "$DESTDIR/share/applications"
cp "$SRC/preso.desktop" "$DESTDIR/share/applications/preso.desktop"
