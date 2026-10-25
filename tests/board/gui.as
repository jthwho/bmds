// The GUI's bindings on the board (devplan/guiprograms.md §13): properties of each type, a list,
// a 64-bit number, an event coming back to its callback, timers and the loop, destroy(), a
// destroyed object's use; an Image as an icon, a dialog's owner (5b); a Canvas drawn as a Screen
// is, a button checked (5c). Run with `angel tests/board/gui.as`; it prints "gui.as: ok".

int failures = 0;
void check(bool ok, const string &in what) {
        if (ok) return;
        println("gui.as: failed: " + what);
        failures++;
}

Window w("angel's GUI test", 360, 140);
Box column(w, "down");
Box row(column, "across");
Label label(row, "Hello");
Button button(row, "&Press");
CheckBox box(row, "A box");
check(label.text == "Hello", "a label's text read back");
label.text = "Changed";
check(label.text == "Changed", "a label's text set");
check(row.direction == "across", "an atom property");
box.checked = true;
check(box.checked, "a bool property");
ComboBox combo(column);
combo.items = {"one", "two", "three"};
array<string>@ items = combo.items;
check(items.length() == 3 && items[1] == "two", "a list property");
ProgressBar bar(column);
bar.maximum = 10000000000;
bar.value = 5000000000;
check(bar.value == 5000000000, "a 64-bit property");
Widget@ asWidget = button;
check(asWidget.enabled, "a button as a Widget");

// An Image as an icon: read back as the same Image; drawn on and set again, its pixels sent again.
Image icon(16, 16);
icon.fillRect(0, 0, 16, 16, Color::Red);
@button.iconImage = icon;
check(button.iconImage is icon, "an icon read back as its Image");
icon.fillRect(4, 4, 8, 8, Color::Blue);
@button.iconImage = icon;
@w.iconImage = icon;
check(w.iconImage is icon, "a window's icon");
@button.iconImage = null;
check(button.iconImage is null, "no icon");
bool tooBig = false;
try {
        @button.iconImage = Image(600, 20);
} catch {
        tooBig = true;
}
check(tooBig, "an icon over 512 pixels raises");

// A dialog's owner: the window it blocks while it is open.
MessageBox message;
@message.owner = w;
check(message.owner is w, "a dialog's owner");
message.destroy();

// A button drawn checked.
button.checked = true;
check(button.checked, "a button checked");
button.checked = false;
check(!button.checked, "a button not checked");

// A Canvas drawn as a Screen is: in memory at the size it asks for until it is laid out, then
// through its surface, the picture so far copied over.
Canvas canvas(column);
canvas.preferredWidth = 80;
canvas.preferredHeight = 30;
canvas.stretch = 0;
canvas.fillRect(0, 0, 10, 10, Color::Red);
check(canvas.pixel(5, 5) == Color::Red, "a canvas drawn before it is shown");
check(canvas.pixel(20, 20) == Color::White, "a canvas starts white");
int canvasResized = 0;
canvas.onResized(function(int width, int height) { canvasResized++; });
Canvas sizeless(column);
bool noSize = false;
try {
        sizeless.plot(0, 0, Color::Black);
} catch {
        noSize = true;
}
check(noSize, "a canvas with no size yet raises");
sizeless.destroy();

int clicked = 0;
button.onClicked(function() { clicked++; });
w.show();
button.click(); // its clicked event comes back to the callback

// Made in a function and let go of: it stays, its callback too.
void addButton() {
        Button later(row, "Later");
        later.onClicked(function() { clicked += 10; });
        later.click();
}
addButton();

int ticks = 0;
app.every(50, function() {
        ticks++;
        if (ticks < 3) return;
        check(clicked == 11, "the clicks' callbacks (" + clicked + ")");
        Rect g = label.geometry;
        check(g.width > 0 && g.height > 0, "a widget's geometry");
        check(canvasResized >= 1, "a canvas's resized");
        check(canvas.pixel(5, 5) == Color::Red, "a canvas's picture kept when it is shown");
        canvas.fillCircle(40, 15, 8, Color::Blue);
        check(canvas.pixel(40, 15) == Color::Blue, "a canvas drawn through its surface");
        Image@ part = canvas.part(0, 0, 10, 10);
        check(part.pixel(5, 5) == Color::Red, "a canvas's part");
        w.close(); // the last window shown: the loop ends
});
int late = app.after(5000, function() {
        check(false, "the loop ended in time");
        app.quit();
});
app.run();
app.stop(late);
check(ticks == 3, "every() three times (" + ticks + ")");

Button doomed(row, "Doomed");
doomed.destroy();
bool raised = false;
try {
        doomed.text = "too late";
} catch {
        raised = true;
}
check(raised, "a destroyed object's use raises");
canvas.destroy();
raised = false;
try {
        canvas.plot(0, 0, Color::Black);
} catch {
        raised = true;
}
check(raised, "a destroyed canvas's drawing raises");
w.destroy();
if (failures == 0) println("gui.as: ok");
