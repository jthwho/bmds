#!/rom/bin/angel
// Serpents: eat the numbers 1 to 9 in turn; each makes you longer. Eat the 9 and the next level
// has new walls, and everything goes faster. Don't hit a wall, yourself or the other serpent.
// One player steers with the arrow keys; a second with W A S D. Press 1 or 2 to start.

const int CELL = 16;
const int COLS = 40, ROWS = 28;
const int TOP = 32; // the score line above the field
Screen w("Serpents", COLS * CELL, ROWS * CELL + TOP);

// The walls: a grid of cells, true where there's a wall.
bool[] walls(COLS * ROWS);

bool wallAt(int x, int y) {
        return x < 0 || y < 0 || x >= COLS || y >= ROWS || walls[y * COLS + x];
}

void wallLine(int x1, int y1, int x2, int y2) {
        for (int x = min(x1, x2); x <= max(x1, x2); x++)
                for (int y = min(y1, y2); y <= max(y1, y2); y++) walls[y * COLS + x] = true;
}

// Each level's walls (after the fifth, they come round again).
void buildLevel(int level) {
        for (uint i = 0; i < walls.length(); i++) walls[i] = false;
        wallLine(0, 0, COLS - 1, 0);
        wallLine(0, ROWS - 1, COLS - 1, ROWS - 1);
        wallLine(0, 0, 0, ROWS - 1);
        wallLine(COLS - 1, 0, COLS - 1, ROWS - 1);
        switch ((level - 1) % 5) {
                case 1: wallLine(10, ROWS / 2, COLS - 11, ROWS / 2); break;
                case 2:
                        wallLine(13, 5, 13, ROWS - 6);
                        wallLine(COLS - 14, 5, COLS - 14, ROWS - 6);
                        break;
                case 3:
                        wallLine(COLS / 2, 4, COLS / 2, ROWS / 2 - 3);
                        wallLine(COLS / 2, ROWS / 2 + 3, COLS / 2, ROWS - 5);
                        wallLine(6, ROWS / 2, COLS / 2 - 5, ROWS / 2);
                        wallLine(COLS / 2 + 5, ROWS / 2, COLS - 7, ROWS / 2);
                        break;
                case 4:
                        for (int i = 0; i < 4; i++) {
                                int bx = i % 2 == 0 ? 8 : COLS - 13, by = i < 2 ? 6 : ROWS - 11;
                                wallLine(bx, by, bx + 4, by);
                                wallLine(bx, by + 4, bx + 4, by + 4);
                        }
                        break;
        }
}

class Serpent {
        int[] xs, ys; // the body, head first
        int dx, dy;
        int[] turnsX, turnsY; // keys pressed, not yet taken (two at most)
        int grow = 0;
        int score = 0;
        int lives = 5;
        uint color;
        bool alive = true;
        string up, down, left, right;

        Serpent() {}
        Serpent(uint c, const string &in u, const string &in d, const string &in l, const string &in r) {
                color = c;
                up = u;
                down = d;
                left = l;
                right = r;
        }

        // Back to the start of a round: short, at @p x, @p y, heading @p ddx.
        void place(int x, int y, int ddx) {
                xs.resize(0);
                ys.resize(0);
                for (int i = 0; i < 3; i++) {
                        xs.insertLast(x - i * ddx);
                        ys.insertLast(y);
                }
                dx = ddx;
                dy = 0;
                turnsX.resize(0);
                turnsY.resize(0);
                grow = 0;
        }

        bool covers(int x, int y) const {
                for (uint i = 0; i < xs.length(); i++)
                        if (xs[i] == x && ys[i] == y) return true;
                return false;
        }

        void key(const string &in k) {
                int nx = 0, ny = 0;
                if (k == up) ny = -1;
                else if (k == down) ny = 1;
                else if (k == left) nx = -1;
                else if (k == right) nx = 1;
                else return;
                // Measured against the last turn waiting, so two quick keys make a U-turn.
                int lx = turnsX.isEmpty() ? dx : turnsX[turnsX.length() - 1];
                int ly = turnsY.isEmpty() ? dy : turnsY[turnsY.length() - 1];
                if ((nx != 0 && lx != 0) || (ny != 0 && ly != 0) || turnsX.length() >= 2) return;
                turnsX.insertLast(nx);
                turnsY.insertLast(ny);
        }

        int headX() const { return xs[0] + dx; }
        int headY() const { return ys[0] + dy; }

        void turn() {
                if (turnsX.isEmpty()) return;
                dx = turnsX[0];
                dy = turnsY[0];
                turnsX.removeAt(0);
                turnsY.removeAt(0);
        }

        void advance() {
                xs.insertAt(0, headX());
                ys.insertAt(0, headY());
                if (grow > 0)
                        grow--;
                else {
                        xs.removeLast();
                        ys.removeLast();
                }
        }
}

class Serpents : Game {
        Serpent[] players;
        int level = 1;
        int number = 1; // the next to eat
        int foodX, foodY;
        int ticks = 0;
        string state = "title"; // "title", "ready", "playing", "over"
        string message;

        void start(int count) {
                players.resize(0);
                players.insertLast(Serpent(Color::Yellow, "Up", "Down", "Left", "Right"));
                if (count == 2) players.insertLast(Serpent(Color::Magenta, "w", "s", "a", "d"));
                level = 1;
                newLevel();
        }

        void newLevel() {
                buildLevel(level);
                number = 1;
                newRound("Level " + level);
        }

        // Everyone back to their places, waiting for a key.
        void newRound(const string &in text) {
                players[0].place(8, ROWS - 4, 1);
                if (players.length() > 1) players[1].place(COLS - 9, 3, -1);
                placeFood();
                message = text + ": press Space";
                state = "ready";
        }

        void placeFood() {
                do {
                        foodX = random(1, COLS - 2);
                        foodY = random(1, ROWS - 2);
                } while (wallAt(foodX, foodY) || onAnyone(foodX, foodY));
        }

        bool onAnyone(int x, int y) {
                foreach (const Serpent@ s : players)
                        if (s.lives > 0 && s.covers(x, y)) return true;
                return false;
        }

        // Moves every eighth tick at level 1, faster each level (to every third).
        int stepTicks() const { return max(3, 9 - level); }

        // Sixty times a second, but the serpents move only every few: false when nothing moved,
        // so the picture isn't drawn again for nothing.
        bool tick() {
                if (state != "playing" || ++ticks % stepTicks() != 0) return false;
                for (uint i = 0; i < players.length(); i++) players[i].turn();
                // Where each head goes, then who hit what.
                bool crashed = false;
                for (uint i = 0; i < players.length(); i++) {
                        Serpent@ s = players[i];
                        if (s.lives == 0) continue;
                        int x = s.headX(), y = s.headY();
                        bool hit = wallAt(x, y) || onAnyone(x, y);
                        for (uint j = 0; j < players.length(); j++)
                                if (j != i && players[j].lives > 0 && players[j].headX() == x && players[j].headY() == y)
                                        hit = true; // head to head
                        if (hit) {
                                s.lives--;
                                crashed = true;
                        }
                }
                if (crashed) {
                        play("MB T120 L8 O2 E C");
                        int left = 0;
                        foreach (const Serpent@ s : players)
                                if (s.lives > 0) left++;
                        if (left == 0 || (players.length() == 2 && left == 1)) {
                                state = "over";
                                message = "Game over! Press 1 or 2 to play again";
                                if (players.length() == 2)
                                        message = (players[0].lives > 0 ? "Yellow" : players[1].lives > 0 ? "Magenta" : "Nobody") + " wins! 1 or 2 plays again";
                        } else {
                                newRound("Ouch");
                        }
                        return true;
                }
                for (uint i = 0; i < players.length(); i++) {
                        Serpent@ s = players[i];
                        if (s.lives == 0) continue;
                        s.advance();
                        if (s.xs[0] == foodX && s.ys[0] == foodY) {
                                s.score += number * level * 10;
                                s.grow += number * 3;
                                play("MB T240 L32 O4 C E G");
                                number++;
                                if (number > 9) {
                                        level++;
                                        play("MB T180 L16 O4 C E G O5 C");
                                        newLevel();
                                        return true;
                                }
                                placeFood();
                        }
                }
                return true;
        }

        void event(Event@ e) {
                if (e.type != KeyDown) return;
                if ((state == "title" || state == "over") && (e.key == "1" || e.key == "2")) {
                        start(e.key == "1" ? 1 : 2);
                        return;
                }
                if (state == "ready" && e.key == "Space") {
                        state = "playing";
                        return;
                }
                if (state == "playing")
                        for (uint i = 0; i < players.length(); i++) players[i].key(e.key);
        }

        void draw() {
                w.clear(Color::Navy);
                // The score line.
                w.fillRect(0, 0, COLS * CELL, TOP, Color::DarkGray);
                for (uint i = 0; i < players.length(); i++)
                        w.text(8 + int(i) * 320, 8, "Score " + players[i].score + "  Lives " + players[i].lives,
                               players[i].color, 1);
                if (!players.isEmpty()) w.text(COLS * CELL - 80, 8, "Level " + level, Color::White, 1);
                if (state == "title") {
                        title();
                        return;
                }
                for (int y = 0; y < ROWS; y++)
                        for (int x = 0; x < COLS; x++)
                                if (walls[y * COLS + x]) w.fillRect(x * CELL, TOP + y * CELL, CELL, CELL, Color::Brown);
                w.fillRect(foodX * CELL, TOP + foodY * CELL, CELL, CELL, Color::Red);
                w.text(foodX * CELL + 4, TOP + foodY * CELL, "" + number, Color::White, 1);
                foreach (const Serpent@ s : players) {
                        if (s.lives == 0) continue;
                        for (uint i = 0; i < s.xs.length(); i++)
                                w.fillRect(s.xs[i] * CELL + 1, TOP + s.ys[i] * CELL + 1, CELL - 2, CELL - 2,
                                           i == 0 ? Color::White : s.color);
                }
                if (state != "playing") banner(message);
        }

        void title() {
                string name = "SERPENTS";
                w.text((COLS * CELL - textWidth(name, 4)) / 2, 120, name, Color::Yellow, 4);
                string[] lines = {"Eat the numbers 1 to 9 in turn.", "Each one makes you longer.",
                                  "Walls, yourself and the other serpent are deadly.", "",
                                  "Player 1: the arrow keys     Player 2: W A S D", "", "Press 1 or 2 for the players"};
                for (uint i = 0; i < lines.length(); i++)
                        w.text((COLS * CELL - textWidth(lines[i])) / 2, 220 + int(i) * 24, lines[i], Color::White);
        }

        void banner(const string &in text) {
                int y = TOP + ROWS * CELL / 2 - 20;
                w.fillRect(0, y, COLS * CELL, 40, Color::Black, 0.7);
                w.text((COLS * CELL - textWidth(text, 2)) / 2, y + 4, text, Color::White, 2);
        }
}

w.run(Serpents(), 60);
