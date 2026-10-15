# Doom: doomgeneric with BMDS's platform layer (programs/doom), as apps/doom.app (run by
# bmds-dist; see dist/README.md; bmds-cc takes the stack from BMDS_STACK). The game data comes
# from dist/freedoom or the user.

cp "$PKGDIR/Makefile" Makefile
$BMDS_MAKE SRC="$SRC" PLATFORM="$ROOT/programs/doom" CC="$CC" doom
app="$DESTDIR/apps/doom.app"
mkdir -p "$app"
cp doom "$app/doom"
cp "$ROOT/programs/doom/app.desktop" "$app/app.desktop"
