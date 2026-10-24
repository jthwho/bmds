#!/rom/bin/angel
// A paint program: drag with the left button to draw, the right button to fill an area.
// Keys 1-8 choose a colour, + and - the brush's size, C clears, S saves paint.png.

Screen w("Paint", 640, 480);
uint[] palette = {Color::Black, Color::Red, Color::Orange, Color::Yellow, Color::Green, Color::Blue,
                  Color::Purple, Color::White};
uint color = Color::Black;
int brush = 4;
int lastX = -1, lastY = -1;

void drawPalette() {
        for (uint i = 0; i < palette.length(); i++) {
                w.fillRect(int(i) * 30 + 5, 5, 25, 25, palette[i]);
                w.rect(int(i) * 30 + 5, 5, 25, 25, palette[i] == color ? Color::Red : Color::Gray);
        }
        w.fillRect(250, 5, 200, 25, Color::White);
        w.text(255, 10, "brush " + brush + "  (1-8 + - C S)", Color::Black);
}

w.clear(Color::White);
drawPalette();
while (!w.closed()) {
        Event@ e = w.waitEvent();
        if (e.type == KeyDown) {
                if (e.text >= "1" && e.text <= "8") color = palette[ord(e.text) - ord("1")];
                else if (e.text == "+") brush = min(brush + 1, 40);
                else if (e.text == "-") brush = max(brush - 1, 1);
                else if (e.key == "c") w.clear(Color::White);
                else if (e.key == "s") {
                        w.save("paint.png");
                        println("saved paint.png");
                }
                drawPalette();
        } else if (e.type == MouseDown && e.button == 2) {
                if (e.y > 35) w.floodFill(e.x, e.y, color);
        } else if (e.type == MouseDown && e.button == 1 && e.y <= 35) {
                int i = e.x / 30;
                if (i < int(palette.length())) color = palette[i];
                drawPalette();
        } else if ((e.type == MouseMove || e.type == MouseDown) && w.mouseDown(1) && e.y > 35 + brush) {
                if (lastX >= 0) {
                        // A line of circles from the last point, so fast strokes stay joined.
                        int steps = max(abs(e.x - lastX), abs(e.y - lastY)) / max(brush / 2, 1) + 1;
                        for (int s = 1; s <= steps; s++)
                                w.fillCircle(lastX + (e.x - lastX) * s / steps, lastY + (e.y - lastY) * s / steps, brush, color);
                } else {
                        w.fillCircle(e.x, e.y, brush, color);
                }
                lastX = e.x;
                lastY = e.y;
        }
        if (!w.mouseDown(1)) lastX = -1;
}
