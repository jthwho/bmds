# preso: the core, the BMDS platform and md4c (thirdparty/md4c), run by bmds-dist (dist/README.md).

cp "$PKGDIR/Makefile" Makefile
$BMDS_MAKE MD4C="$ROOT/thirdparty/md4c" SRC="$SRC" CC="$CC" CXX="$CXX" VERSION="$VERSION" preso
mkdir -p "$DESTDIR/bin"
cp preso "$DESTDIR/bin/preso"
