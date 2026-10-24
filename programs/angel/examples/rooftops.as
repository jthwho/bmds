#!/rom/bin/angel
// Rooftops: two throwers on a city's roofs take turns throwing a bomb at each other. Type the
// angle (0 to 90) and Enter, then the speed (1 to 100) and Enter, and watch it fly. Gravity
// pulls it down and the wind pushes it; the buildings are knocked about where it lands. The
// first to three hits wins.

const int WIDTH = 640, HEIGHT = 480;
const uint SKY = rgb(30, 30, 110); // the city's sky: where there's no building
const float GRAVITY = 0.2;
Screen w("Rooftops", WIDTH, HEIGHT);

// The city, sky and all: an image, so a bomb can knock holes in it and the flight can ask what's
// there. Without a see-through colour it's copied to the window whole, by the computer's DMA
// rather than its processor.
Image@ city = Image(WIDTH, HEIGHT);

class Thrower {
        int x, y; // feet
        int score = 0;
        uint color;
        Thrower() {}
        Thrower(uint c) { color = c; }
        bool hitBy(float bx, float by) const { return abs(bx - x) < 10 && by > y - 26 && by < y; }
        void draw(bool arm) const {
                w.fillRect(x - 6, y - 18, 12, 18, color);       // body
                w.fillCircle(x, y - 22, 6, color);               // head
                w.line(x - 6, y - 14, x - 12, arm ? y - 26 : y - 6, color); // arms
                w.line(x + 6, y - 14, x + 12, arm ? y - 26 : y - 6, color);
        }
}

class Rooftops : Game {
        Thrower[] players = {Thrower(Color::Orange), Thrower(Color::Lime)};
        int[] roofs;      // each building's top, by column
        int turn = 0;     // whose throw
        float wind = 0;
        string typing = "";
        string angleText = "";
        bool flying = false;
        float bx, by, vx, vy; // the bomb
        int explodeX, explodeY, explodeTime = 0;
        string message = "";
        bool over = false;

        Rooftops() { newCity(); }

        // A new skyline, the throwers on the second building from each side, a new wind.
        void newCity() {
                city.clear(SKY);
                roofs.resize(WIDTH);
                int x = 0;
                int first = 0, last = 0;
                while (x < WIDTH) {
                        int width = random(50, 80);
                        int top = random(180, 400);
                        uint color = random(0, 2) == 0 ? Color::Gray : random(0, 1) == 0 ? rgb(160, 60, 60) : Color::Sky;
                        city.fillRect(x, top, width - 2, HEIGHT - top, color);
                        // Windows, lit or not.
                        for (int wy = top + 8; wy < HEIGHT - 12; wy += 18)
                                for (int wx = x + 6; wx < x + width - 12; wx += 12)
                                        city.fillRect(wx, wy, 5, 9, random(0, 3) == 0 ? Color::DarkGray : Color::Yellow);
                        for (int c = x; c < x + width && c < WIDTH; c++) roofs[c] = top;
                        if (x < 100) first = x + width / 2;
                        if (x < WIDTH - 140) last = x + width / 2;
                        x += width;
                }
                players[0].x = max(first, 60);
                players[1].x = min(last + 80, WIDTH - 60);
                for (uint i = 0; i < players.length(); i++) players[i].y = roofs[players[i].x];
                wind = random(-10, 10) / 10.0;
                typing = "";
                angleText = "";
        }

        void throwBomb(float angle, float speed) {
                float radians = angle * PI / 180;
                float power = speed / 7;
                bx = players[turn].x;
                by = players[turn].y - 30;
                vx = cos(radians) * power * (turn == 0 ? 1 : -1);
                vy = -sin(radians) * power;
                flying = true;
                play("MB T200 L32 O3 C");
        }

        // False when nothing moved (waiting for the angle and speed): the picture stays as it is.
        bool tick() {
                bool moved = explodeTime > 0;
                if (explodeTime > 0) explodeTime--;
                if (!flying) return moved;
                // A few small steps a tick, so a fast bomb can't jump over a thin wall.
                for (int step = 0; step < 4 && flying; step++) {
                        vy += GRAVITY / 4;
                        vx += wind * 0.01;
                        bx += vx / 4;
                        by += vy / 4;
                        if (bx < 0 || bx >= WIDTH || by >= HEIGHT) {
                                flying = false;
                                message = "Missed!";
                                nextTurn();
                                return true;
                        }
                        for (uint i = 0; i < players.length(); i++) {
                                if (!players[i].hitBy(bx, by)) continue;
                                flying = false;
                                int scorer = i == uint(turn) ? 1 - turn : turn; // hitting yourself scores for the other
                                players[scorer].score++;
                                explode(int(bx), int(by), 40);
                                play("MB T150 L16 O2 C O1 G E C");
                                if (players[scorer].score == 3) {
                                        over = true;
                                        message = (scorer == 0 ? "Orange" : "Green") + " wins! Space plays again";
                                } else {
                                        message = i == uint(turn) ? "Oops! Hit yourself." : "A hit!";
                                        newCity();
                                        turn = 1 - scorer; // the one hit throws first
                                }
                                return true;
                        }
                        if (by >= 0 && city.pixel(int(bx), int(by)) != SKY) {
                                flying = false;
                                explode(int(bx), int(by), 16);
                                play("MB T200 L32 O2 C");
                                message = "";
                                nextTurn();
                                return true;
                        }
                }
                return true;
        }

        void explode(int x, int y, int radius) {
                city.fillCircle(x, y, radius, SKY);
                explodeX = x;
                explodeY = y;
                explodeTime = 20;
        }

        void nextTurn() {
                turn = 1 - turn;
                typing = "";
                angleText = "";
        }

        void event(Event@ e) {
                if (e.type != KeyDown) return;
                if (over) {
                        if (e.key == "Space") {
                                players[0].score = players[1].score = 0;
                                over = false;
                                message = "";
                                turn = 0;
                                newCity();
                        }
                        return;
                }
                if (flying) return;
                if (e.key == "Backspace" && typing.length() > 0) {
                        typing.erase(typing.length() - 1);
                } else if (e.key == "Enter" && typing.length() > 0) {
                        if (angleText == "") {
                                angleText = "" + clamp(int(parseInt(typing)), 0, 90);
                                typing = "";
                        } else {
                                int speed = clamp(int(parseInt(typing)), 1, 100);
                                throwBomb(float(parseInt(angleText)), float(speed));
                                message = "";
                        }
                } else if (e.text.length() == 1 && e.text[0] >= 48 && e.text[0] <= 57 && typing.length() < 3) {
                        typing += e.text; // a digit
                }
        }

        void draw() {
                w.image(city, 0, 0);
                w.fillCircle(WIDTH / 2, 50, 24, Color::Yellow); // the sun, above any building
                for (uint i = 0; i < players.length(); i++) players[i].draw(!flying && int(i) == turn && !over);
                if (flying) w.fillCircle(int(bx), int(by), 4, Color::White);
                if (explodeTime > 0) w.fillCircle(explodeX, explodeY, 40 - explodeTime, mix(Color::Yellow, Color::Red, explodeTime / 20.0));
                // The scores, the wind, and whose turn.
                w.text(8, 8, "Orange " + players[0].score, Color::Orange, 2);
                string green = "Green " + players[1].score;
                w.text(WIDTH - 8 - textWidth(green, 2), 8, green, Color::Lime, 2);
                int wx = WIDTH / 2, wy = 104; // the wind, under the sun
                int length = int(wind * 40);
                w.line(wx, wy, wx + length, wy, Color::White);
                if (length != 0) w.fillPolygon({wx + length, wy - 4, wx + length + (length > 0 ? 6 : -6), wy, wx + length, wy + 4}, Color::White);
                w.text(wx - textWidth("wind") / 2, wy - 24, "wind", Color::White);
                if (!flying && !over) {
                        string prompt = angleText == "" ? "Angle: " + typing + "_" : "Angle: " + angleText + "  Speed: " + typing + "_";
                        int px = turn == 0 ? 8 : WIDTH - 8 - textWidth(prompt);
                        w.text(px, 40, prompt, Color::White);
                }
                if (message != "") w.text((WIDTH - textWidth(message, 2)) / 2, 130, message, Color::White, 2);
        }
}

w.run(Rooftops(), 60);
