#!/rom/bin/angel
// A bouncing ball: draw, move, wait for the next frame, again. Close the window to stop.

Screen w("Bounce", 640, 480);
float x = 320, y = 100, dx = 4, dy = 0;
float gravity = 0.4;
int radius = 24;

while (true) {
        w.clear(Color::Navy);
        w.fillRect(0, 460, 640, 20, Color::Green);
        w.fillCircle(int(x), int(y), radius, Color::Orange);
        w.text(10, 10, "Bounce!", Color::White, 2);
        dy += gravity;
        x += dx;
        y += dy;
        if (x < radius || x > 640 - radius) dx = -dx;
        if (y > 460 - radius) {
                y = 460 - radius;
                dy = -dy * 0.9; // each bounce a little lower
        }
        w.frame();
}
