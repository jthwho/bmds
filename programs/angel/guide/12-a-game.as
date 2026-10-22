#!/rom/bin/angel
// Lesson 12: A game
//
// Games are made of the same parts: things that move each moment (tick), a picture of how
// things are (draw), and the player's keys and clicks (event). A class that is a Game has those
// three methods, and w.run() calls them at the right times: tick() so many times a second,
// event() for each key or click, and draw() after them, when the picture may have changed.
// (Between ticks the computer rests: nothing moves, so there's nothing new to draw.)
//
// Catch the falling stars with the basket (the arrow keys or the mouse). Miss three and the game
// is over; Space plays again.

Window w("Star Catcher", 640, 480);

// A falling star: where it is and how fast it falls. A small class of its own.
class Star {
        float x, y, speed;
        Star() {}
        Star(float sx, float sp) {
                x = sx;
                y = -10;
                speed = sp;
        }
}

// The game: everything it needs to remember, and the three methods.
class StarCatcher : Game {
        float basketX = 280;
        int basketWidth = 80;
        Star[] stars;
        int score = 0;
        int lives = 3;
        int ticks = 0;
        bool over = false;

        void tick() {
                if (over) return;
                ticks++;
                // A new star now and then, more often as the score goes up.
                if (ticks % max(15, 50 - score) == 0)
                        stars.insertLast(Star(random(10, 630), 2 + score * 0.1));
                if (w.keyDown("Left")) basketX -= 7;
                if (w.keyDown("Right")) basketX += 7;
                basketX = clamp(basketX, 0, 640 - basketWidth);
                // Move each star; caught or missed, it goes. Backwards, so removing one doesn't
                // skip the next.
                for (int i = int(stars.length()) - 1; i >= 0; i--) {
                        Star@ s = stars[i];
                        s.y += s.speed;
                        if (s.y >= 440 && s.y < 460 && s.x >= basketX && s.x <= basketX + basketWidth) {
                                score++;
                                play("MB T200 L32 O5 C G");
                                stars.removeAt(i);
                        } else if (s.y > 480) {
                                lives--;
                                play("MB T120 L16 O2 C");
                                stars.removeAt(i);
                                if (lives == 0) over = true;
                        }
                }
        }

        void draw() {
                w.clear(Color::Navy);
                foreach (Star s : stars) w.fillCircle(int(s.x), int(s.y), 8, Color::Yellow);
                w.fillRect(int(basketX), 450, basketWidth, 16, Color::Orange);
                w.text(10, 10, "Score: " + score + "   Lives: " + lives, Color::White, 2);
                if (over) {
                        string message = "Game over! Space plays again";
                        w.text((640 - textWidth(message, 2)) / 2, 220, message, Color::White, 2);
                }
        }

        void event(Event@ e) {
                // The mouse moves the basket too.
                if (e.type == MouseMove) basketX = e.x - basketWidth / 2;
                if (over && e.type == KeyDown && e.key == "Space") {
                        stars.resize(0);
                        score = 0;
                        lives = 3;
                        over = false;
                }
        }
}

// Sixty ticks a second, until the window is closed.
w.run(StarCatcher(), 60);

// Try this:
//   1. Give a bonus life every 10 stars.
//   2. Add red bombs that fall too, and end the game if caught.
//   3. Keep the best score while the game runs, and show it beside the score.
