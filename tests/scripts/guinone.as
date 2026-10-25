// The GUI's bindings without a display (the host has none): a script using them builds, and
// making an object or asking with a dialog raises "No GUI here" (devplan/guiprograms.md §13).
void build() {
        Window w("Test", 200, 100);
        Box row(w, "across");
        Label l(row, "Clicks: 0");
        Button b(row, "&Click");
        b.onClicked(function() { println("clicked"); });
        LineEdit e(row);
        e.onAccepted(function(text) { println(text); });
        Widget@ asWidget = b;
        Rect r = b.geometry;
        app.every(1000, function() { println("tick"); });
        w.show();
        app.run();
}
Rect r(1, 2, 3, 4);
println("a rect: " + r.x + " " + r.y + " " + r.width + " " + r.height);
try {
        build();
} catch {
        println(getExceptionInfo());
}
// The one-line dialogs (5b) raise the same; an image property's type is angel's Image.
try {
        alert("hello");
} catch {
        println(getExceptionInfo());
}
try {
        println(chooseFile());
} catch {
        println(getExceptionInfo());
}
void icons(Button@ b) {
        Image i(16, 16);
        @b.iconImage = i;
        Image@ back = b.iconImage;
}
// A canvas has a Screen's drawing methods, a screen's events a callback in the loop, and colours
// a dialog of their own (5c); a screen's part is what is drawn on it.
try {
        println(chooseColor(Color::Red));
} catch {
        println(getExceptionInfo());
}
void drawing(Canvas@ c, Screen@ s) {
        c.fillRect(0, 0, 10, 10, Color::Red);
        c.text(2, 2, "hi", Color::Black);
        Image@ part = c.part(0, 0, 4, 4);
        c.image(part, 10, 10);
        uint p = c.pixel(1, 1) + c.width;
        s.onEvent(function(e) { println(e.key); });
}
Screen s("part", 20, 20);
s.fillRect(0, 0, 5, 5, Color::Red);
Image@ grabbed = s.part(0, 0, 8, 8);
println("a screen's part: " + (grabbed.pixel(2, 2) == Color::Red) + " " + (grabbed.pixel(6, 6) == Color::Black));
