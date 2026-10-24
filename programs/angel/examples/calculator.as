// A calculator in a window of its own (the GUI's classes: devplan/guiprograms.md §13): a display
// and rows of buttons that share the window as it is resized (each row's stretch, each button's).
// Each button is a Key that remembers its label, so one function serves them all: the key's
// method is its button's callback (a delegate, Callback(this.pressed)).

Window w("Calculator", 240, 280);
w.minWidth = 200;
w.minHeight = 220;
w.margin = 8;
w.spacing = 6;
Label display(w, "0");
display.align = "right";
display.frame = "field";

double total = 0;     // what the operator works on
string pending = "";  // the operator waiting for its second number
string entry = "0";   // the number being typed
bool typing = false;  // a digit was typed since the last operator

void show(const string &in text) {
        display.text = text;
}

// The number as people write it: no ".0" on whole numbers.
string written(double v) {
        if (v == double(int64(v)) && v < 1e15 && v > -1e15) return "" + int64(v);
        return "" + v;
}

void apply() {
        const double x = parseDouble(entry);
        if (pending == "+")
                total += x;
        else if (pending == "-")
                total -= x;
        else if (pending == "*")
                total *= x;
        else if (pending == "/")
                total = x != 0 ? total / x : 0;
        else
                total = x;
        entry = written(total);
        return;
}

void press(const string &in key) {
        if (key == "C") {
                total = 0;
                pending = "";
                entry = "0";
                typing = false;
        } else if (key == "=") {
                apply();
                pending = "";
                typing = false;
        } else if (key == "+" || key == "-" || key == "*" || key == "/") {
                if (typing || pending == "") apply();
                pending = key;
                typing = false;
        } else if (key == ".") {
                if (!typing) entry = "0";
                if (entry.find(".") < 0) entry += ".";
                typing = true;
        } else {
                entry = typing && entry != "0" ? entry + key : key;
                typing = true;
        }
        show(entry);
        return;
}

class Key {
        string label;

        Key(Box@ row, const string &in text) {
                label = text;
                Button button(row, text);
                button.centered = true;
                button.stretch = 1; // the row's width shared
                button.onClicked(Callback(this.pressed));
        }

        void pressed() {
                press(label);
        }
}

array<Key@> keys;
const array<string> rows = {"7 8 9 /", "4 5 6 *", "1 2 3 -", "0 . = +", "C"};
for (uint r = 0; r < rows.length(); ++r) {
        Box row(w, "across");
        row.spacing = 6;
        row.stretch = 1; // the window's height shared
        array<string>@ labels = rows[r].split(" ");
        for (uint k = 0; k < labels.length(); ++k) keys.insertLast(Key(row, labels[k]));
}

w.onClosed(function() { app.quit(); });
w.show();
app.run();
