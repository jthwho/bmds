# angel: AngelScript (thirdparty/angelscript, patched) and the runtime (run by bmds-dist; see
# dist/README.md). ANGEL_FLOAT_LITERALS=double builds it with AngelScript's own literals
# (devplan/scripting.md §4); the default is float.

AS="$ROOT/thirdparty/angelscript"
DEFINES="-DANGEL_BMDS -DAS_TOP_LEVEL_STATEMENTS -DAS_NO_EXCEPTIONS -DANGEL_VERSION=\\\"$VERSION\\\""
case "${ANGEL_FLOAT_LITERALS:-float}" in
        float) DEFINES="$DEFINES -DAS_FLOAT_LITERALS" ;;
        double) ;;
        *) echo "ANGEL_FLOAT_LITERALS must be float or double" >&2; exit 1 ;;
esac
cp "$PKGDIR/Makefile" Makefile
$BMDS_MAKE AS="$AS" SRC="$SRC" CXX="$CXX" DEFINES="$DEFINES" angel
mkdir -p "$DESTDIR/bin" "$DESTDIR/share/licenses/angel"
cp angel "$DESTDIR/bin/angel"
cp "$AS/LICENSE" "$DESTDIR/share/licenses/angel/angelscript.LICENSE"
