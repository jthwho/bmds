// The GUI's bindings on the board (devplan/guiprograms.md §13): properties of each type, a list,
// a 64-bit number, an event coming back to its callback, timers and the loop, destroy(), a
// destroyed object's use. Run with `angel tests/board/gui.as`; it prints "gui.as: ok".

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
w.destroy();
if (failures == 0) println("gui.as: ok");
