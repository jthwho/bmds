#!/rom/bin/angel
// The system's fonts: each family written in itself, at a size you choose. Up and Down change the
// size, S changes the smoothing, B bold, I italic. Close the window to stop.

Screen w("Fonts", 800, 480);
string[]@ families = fontFamilies();
int size = 20;
int style = Regular;
TextSmoothing smooth = TextSmoothing::Default;
string[] smoothing = {"as the system says", "smooth", "sharp"};

void draw() {
        w.clear(Color::White);
        Font title("title");
        int y = 8;
        w.text(10, y, "Fonts at " + size + " pixels, " + smoothing[smooth == Smooth ? 1 : smooth == Sharp ? 2 : 0],
               Color::Navy, title);
        y += title.lineHeight + 8;
        for (uint i = 0; i < families.length() && y < w.height(); i++) {
                Font f(families[i], size, style);
                int x = 10 + w.text(10, y, f.name, Color::Black, f, smooth);
                w.text(x, y, "  The quick brown fox jumps over the lazy dog", Color::Gray, f, smooth);
                y += f.lineHeight + 4;
        }
        Font hint("ui");
        w.text(10, w.height() - hint.lineHeight - 4, " Up, Down: size   S: smoothing   B: bold   I: italic ",
               Color::White, Color::Navy, hint);
}

while (true) {
        draw();
        string k = w.key();
        if (k == "Up" && size < 64) size += 2;
        else if (k == "Down" && size > 8) size -= 2;
        else if (k == "s" || k == "S")
                smooth = smooth == TextSmoothing::Default ? Smooth : smooth == Smooth ? Sharp : TextSmoothing::Default;
        else if (k == "b" || k == "B") style ^= Bold;
        else if (k == "i" || k == "I") style ^= Italic;
}
