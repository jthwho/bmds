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
