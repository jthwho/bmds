#!/rom/bin/angel
// Snake: steer with the arrow keys (or WASD, or a gamepad), eat the apples, don't hit the walls or
// yourself. A game in Screen.run(): tick() moves the snake ten times a second, and draw() shows each
// move. The sounds are play()'s notes, in the background ("MB") so the game goes on.
//
// What it can do is declared as actions, each with its keys and pad controls: the system matches
// them and the game gets the action, so the user can bind other keys (the Controls window) and
// nothing here changes.

const int CELL = 16;
const int COLS = 40, ROWS = 28;
Screen w("Snake", COLS * CELL, ROWS * CELL + 24);

controls.declare("""
[Action left]
Label=Turn left
Keys=Left;A
Pad=Pad.Left;Pad.LeftX-

[Action right]
Label=Turn right
Keys=Right;D
Pad=Pad.Right;Pad.LeftX+

[Action up]
Label=Turn up
Keys=Up;W
Pad=Pad.Up;Pad.LeftY-

[Action down]
Label=Turn down
Keys=Down;S
Pad=Pad.Down;Pad.LeftY+

[Action again]
Label=Play again
Keys=Space;Enter
Pad=Pad.South;Pad.Start
""");

// The first key bound to an action, for a hint ("Space").
string firstKey(const string &in action) {
        string keys = controls.keys(action);
        return keys.length() == 0 ? action : keys.split(";")[0].trim();
}

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
                if (e.type != ActionDown) return;
                if (over && e.action == "again") {
                        Snake fresh;
                        this = fresh; // start again
                        return;
                }
                // No turning straight back into yourself.
                if (e.action == "left" && dx == 0) { nextDx = -1; nextDy = 0; }
                if (e.action == "right" && dx == 0) { nextDx = 1; nextDy = 0; }
                if (e.action == "up" && dy == 0) { nextDx = 0; nextDy = -1; }
                if (e.action == "down" && dy == 0) { nextDx = 0; nextDy = 1; }
        }

        void tick() {
                if (over) return;
                dx = nextDx;
                dy = nextDy;
                int x = xs[0] + dx, y = ys[0] + dy;
                if (x < 0 || y < 0 || x >= COLS || y >= ROWS || onSnake(x, y)) {
                        over = true;
                        play("MB T100 I80 O2 L8 G E C");
                        return;
                }
                xs.insertAt(0, x);
                ys.insertAt(0, y);
                if (x == appleX && y == appleY) {
                        score++;
                        play("MB T240 I80 O4 L32 C E G");
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
                        string message = "Game over! " + firstKey("again") + " to play again";
                        w.fillRect(0, ROWS * CELL / 2 - 20, COLS * CELL, 40, Color::Black, 0.7);
                        w.text((COLS * CELL - textWidth(message, 2)) / 2, ROWS * CELL / 2 - 16, message, Color::White, 2);
                }
        }
}

w.run(Snake(), 10);
