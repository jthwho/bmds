#!/rom/bin/angel
// Snake: steer with the arrow keys, eat the apples, don't hit the walls or yourself.
// A game in Window.run(): tick() moves the snake ten times a second, draw() draws each frame.

const int CELL = 16;
const int COLS = 40, ROWS = 28;
Window w("Snake", COLS * CELL, ROWS * CELL + 24);

class Snake : Game {
        int[] xs, ys;      // the body, head first
        int dx = 1, dy = 0;
        int nextDx = 1, nextDy = 0;
        int appleX, appleY;
        int score = 0;
        bool over = false;

        Snake() {
                for (int i = 0; i < 4; i++) {
                        xs.insertLast(10 - i);
                        ys.insertLast(14);
                }
                placeApple();
        }

        void placeApple() {
                do {
                        appleX = random(0, COLS - 1);
                        appleY = random(0, ROWS - 1);
                } while (onSnake(appleX, appleY));
        }

        bool onSnake(int x, int y) {
                for (uint i = 0; i < xs.length(); i++)
                        if (xs[i] == x && ys[i] == y) return true;
                return false;
        }

        void event(Event@ e) {
                if (e.type != KeyDown) return;
                if (over && e.key == "Space") {
                        Snake fresh;
                        this = fresh; // start again
                        return;
                }
                // No turning straight back into yourself.
                if (e.key == "Left" && dx == 0) { nextDx = -1; nextDy = 0; }
                if (e.key == "Right" && dx == 0) { nextDx = 1; nextDy = 0; }
                if (e.key == "Up" && dy == 0) { nextDx = 0; nextDy = -1; }
                if (e.key == "Down" && dy == 0) { nextDx = 0; nextDy = 1; }
        }

        void tick() {
                if (over) return;
                dx = nextDx;
                dy = nextDy;
                int x = xs[0] + dx, y = ys[0] + dy;
                if (x < 0 || y < 0 || x >= COLS || y >= ROWS || onSnake(x, y)) {
                        over = true;
                        return;
                }
                xs.insertAt(0, x);
                ys.insertAt(0, y);
                if (x == appleX && y == appleY) {
                        score++;
                        placeApple();
                } else {
                        xs.removeLast();
                        ys.removeLast();
                }
        }

        void draw() {
                w.clear(Color::Black);
                w.fillRect(0, ROWS * CELL, COLS * CELL, 24, Color::DarkGray);
                w.text(8, ROWS * CELL + 4, "Score: " + score, Color::White);
                w.fillCircle(appleX * CELL + CELL / 2, appleY * CELL + CELL / 2, CELL / 2 - 1, Color::Red);
                for (uint i = 0; i < xs.length(); i++)
                        w.fillRect(xs[i] * CELL + 1, ys[i] * CELL + 1, CELL - 2, CELL - 2, i == 0 ? Color::Yellow : Color::Lime);
                if (over) {
                        string message = "Game over! Space to play again";
                        w.fillRect(0, ROWS * CELL / 2 - 20, COLS * CELL, 40, Color::Black, 0.7);
                        w.text((COLS * CELL - textWidth(message, 2)) / 2, ROWS * CELL / 2 - 16, message, Color::White, 2);
                }
        }
}

w.run(Snake(), 10);
