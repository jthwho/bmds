# Lucky Duck: an application folder (run by bmds-dist; see dist/README.md), its pictures, sounds
# and icons drawn by assets/build.py, which also writes art.h and art.c (where each picture is).

app="$DESTDIR/apps/luckyduck.app"
mkdir -p "$app"
python3 "$SRC/assets/build.py" --out "$app" --code "$BUILD"
python3 "$ROOT/sdk/tools/bmds-icon-notes" -o "$BUILD/icons.S" luckyduck "$BUILD/icon-16.png" "$BUILD/icon-32.png"
"$CC" -O2 -Wall -Wextra -Werror -I"$BUILD" -o "$app/luckyduck" "$SRC/main.c" "$SRC/lobby.c" "$SRC/solitaire.c" \
        "$SRC/solitaire_rules.c" "$SRC/blackjack.c" "$SRC/blackjack_rules.c" "$SRC/dice.c" "$SRC/dice_rules.c" "$SRC/video.c" "$SRC/video_rules.c" "$SRC/poker.c" "$SRC/poker_rules.c" "$SRC/poker_bot.c" "$SRC/hand.c" "$SRC/deck.c" "$SRC/stage.c" "$SRC/ui.c" "$SRC/data.c" "$BUILD/art.c" "$BUILD/icons.S"
cp "$SRC/app.desktop" "$app/app.desktop"
