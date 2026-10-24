#!/rom/bin/angel
// Lesson 11: Animation
//
// Moving pictures are still pictures shown one after another, fast. A program draws a picture
// (a frame), shows it with w.frame(), changes a little, and draws the next. w.frame() waits for
// the screen, so this goes about 60 times a second.
//
// Steer the blue square with the arrow keys to catch the yellow coin. Close the window to stop.

Screen w("Catch the coin", 640, 480);

int x = 300, y = 220;  // the player's top-left corner
int size = 40;
int speed = 5;
int coinX = 100, coinY = 100;
int score = 0;

// w.closed() becomes true when the window is closed: the loop, and the program, end.
while (!w.closed()) {
        // keyDown says whether a key is held down now. Arrow keys are "Left", "Right", "Up" and
        // "Down"; letters are themselves ("a").
        if (w.keyDown("Left")) x -= speed;
        if (w.keyDown("Right")) x += speed;
        if (w.keyDown("Up")) y -= speed;
        if (w.keyDown("Down")) y += speed;

        // Keep the player in the window: clamp holds a number between two others.
        x = clamp(x, 0, w.width() - size);
        y = clamp(y, 0, w.height() - size);

        // Touching the coin? Its middle is inside the square: a point, and it jumps elsewhere.
        if (coinX >= x && coinX < x + size && coinY >= y && coinY < y + size) {
                score++;
                beep();
                coinX = random(20, w.width() - 20);
                coinY = random(20, w.height() - 20);
        }

        // Draw everything again, from the background up.
        w.clear(Color::Black);
        w.fillCircle(coinX, coinY, 10, Color::Yellow);
        w.fillRect(x, y, size, size, Color::Blue);
        w.text(10, 10, "Score: " + score, Color::White, 2);
        w.frame();
}

// Try this:
//   1. Make the player faster, or the coin bigger.
//   2. Hold Space to go twice as fast: if (w.keyDown("Space")) ...
//   3. Make the coin move by itself, a little each frame, bouncing off the edges (the Bounce
//      example in Help > Examples shows how).
