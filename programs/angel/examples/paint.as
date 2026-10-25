#!/rom/bin/angel
// A paint program in windows of its own (the GUI's classes: devplan/guiprograms.md §13). The
// picture is a Canvas, drawn with a Screen's methods; a Tools window beside it holds the tools,
// the colours and the brush's size. The left button paints in the first colour, the right in the
// second. The one-line dialogs do the asking (chooseFile(), chooseSaveFile(), chooseColor(), ask()).

Window w("Paint", 640, 480);
w.minWidth = 320;
w.minHeight = 240;

MenuBar bar(w);
Menu fileMenu(bar, "&File");
MenuItem newItem(fileMenu, "&New");
MenuItem openItem(fileMenu, "&Open...");
MenuItem saveItem(fileMenu, "&Save");
MenuItem saveAsItem(fileMenu, "Save &As...");
fileMenu.addSeparator();
MenuItem quitItem(fileMenu, "&Quit");
Menu editMenu(bar, "&Edit");
MenuItem undoItem(editMenu, "&Undo");
editMenu.addSeparator();
MenuItem clearItem(editMenu, "&Clear");
Menu windowMenu(bar, "&Window");
MenuItem toolsItem(windowMenu, "&Tools");
Menu helpMenu(bar, "&Help");
MenuItem aboutItem(helpMenu, "&About Paint");

Canvas canvas(w); // stretches: the window's room
StatusBar status(w);
int nameField = 0;
int placeField = 0;
int sizeField = 0;
nameField = status.addField(0);
placeField = status.addField(90);
sizeField = status.addField(90);

// ---- The tools, and their pictures

enum Tool { Pencil, Brush, Line, Rectangle, Ellipse, Fill, Eraser }
const string[] toolNames = {"Pencil", "Brush", "Line", "Rectangle", "Ellipse", "Fill", "Eraser"};

Image@ toolIcon(Tool tool) {
        Image i(16, 16);
        i.clear(Color::Magenta);
        switch (tool) {
        case Pencil:
                i.fillPolygon({3, 10, 10, 3, 13, 6, 6, 13}, 0xf0c000);
                i.polygon({3, 10, 10, 3, 13, 6, 6, 13}, 0x806000);
                i.fillPolygon({3, 10, 6, 13, 1, 15}, Color::Black);
                break;
        case Brush:
                i.line(14, 1, 8, 7, 0x8b4513);
                i.line(13, 1, 7, 7, 0x8b4513);
                i.fillCircle(5, 10, 3, Color::Blue);
                i.fillPolygon({2, 11, 4, 13, 1, 15}, Color::Blue);
                break;
        case Line:
                i.line(2, 13, 13, 2, Color::Black);
                i.line(3, 13, 13, 3, Color::Black);
                break;
        case Rectangle:
                i.rect(2, 4, 12, 9, Color::Black);
                i.rect(3, 5, 10, 7, Color::Black);
                break;
        case Ellipse:
                i.ellipse(8, 8, 6, 4, Color::Black);
                i.ellipse(8, 8, 5, 3, Color::Black);
                break;
        case Fill:
                i.fillPolygon({2, 7, 7, 2, 12, 7, 7, 12}, Color::LightGray);
                i.polygon({2, 7, 7, 2, 12, 7, 7, 12}, Color::Black);
                i.fillRect(2, 7, 11, 2, Color::Blue);
                i.fillCircle(13, 12, 2, Color::Blue);
                break;
        case Eraser:
                i.fillPolygon({1, 10, 8, 3, 14, 9, 7, 15}, Color::Pink);
                i.polygon({1, 10, 8, 3, 14, 9, 7, 15}, Color::Gray);
                i.line(4, 7, 10, 13, Color::Gray);
                break;
        }
        i.transparent(Color::Magenta);
        return i;
}

// ---- The Tools window: beside the picture's, the size of what it holds

Window tools("Tools");
tools.resizable = false;
tools.buttons = false; // no minimize or maximize box
tools.margin = 6;
tools.spacing = 6;
@tools.iconImage = toolIcon(Brush);

ToolBar toolBar(tools);
ToolButton@[] toolButtons;
for (int t = Pencil; t <= Eraser; t++) {
        ToolButton b(toolBar);
        @b.iconImage = toolIcon(Tool(t));
        toolButtons.insertLast(b);
}
Label toolLabel(tools, "Pencil");

// The colours to choose from: two rows of cells; the two in use below them.
const uint[] colors = {Color::Black, Color::DarkGray, Color::Gray, Color::Red, Color::Orange,
                       Color::Yellow, Color::Lime, Color::Cyan, Color::Blue,
                       Color::White, Color::LightGray, Color::Brown, Color::Pink, Color::Magenta,
                       Color::Purple, Color::Green, Color::Sky, Color::Navy};
const int cell = 18;
const int columns = 9;
Canvas swatches(tools);
swatches.preferredWidth = columns * cell + 1;
swatches.preferredHeight = 2 * cell + 1;
swatches.stretch = 0;

Box colorRow(tools, "across");
colorRow.spacing = 8;
Canvas chosen(colorRow);
chosen.preferredWidth = 40;
chosen.preferredHeight = 40;
chosen.stretch = 0;
Button moreButton(colorRow, "&More...");

Box sizeRow(tools, "across");
sizeRow.spacing = 6;
Label sizeLabel(sizeRow, "Size 4");
Slider sizeSlider(sizeRow);
sizeSlider.minimum = 1;
sizeSlider.maximum = 40;
sizeSlider.stretch = 1;

// ---- What is chosen

Tool tool = Pencil;
uint first = Color::Black;   // the left button's colour
uint second = Color::White;  // the right button's (and the eraser's)
int size = 4;

void drawSwatches() {
        swatches.clear(Color::LightGray);
        for (uint i = 0; i < colors.length(); i++) {
                const int x = int(i) % columns * cell, y = int(i) / columns * cell;
                swatches.fillRect(x + 1, y + 1, cell - 1, cell - 1, colors[i]);
                swatches.rect(x, y, cell + 1, cell + 1, Color::Gray);
        }
}

void drawChosen() {
        chosen.clear(Color::LightGray);
        chosen.fillRect(14, 14, 24, 24, second);
        chosen.rect(14, 14, 24, 24, Color::Black);
        chosen.fillRect(2, 2, 24, 24, first);
        chosen.rect(2, 2, 24, 24, Color::Black);
}

void choose(Tool t) {
        tool = t;
        for (uint i = 0; i < toolButtons.length(); i++) toolButtons[i].checked = int(i) == int(t);
        toolLabel.text = toolNames[t];
}

// ---- The picture

string path = "";      // "" until opened or saved
bool modified = false;
int canvasWidth = 0;   // its size, as told (reading it is a round trip)
int canvasHeight = 0;
Image@ undo = null;    // the picture before the last change

string baseName(const string &in p) {
        return p.substr(p.findLast("/") + 1);
}

void showName() {
        const string name = path.isEmpty() ? "Untitled" : baseName(path);
        w.title = (modified ? "*" : "") + name + " - Paint";
        status.setField(nameField, path.isEmpty() ? "Not saved yet" : path);
}

// Kept to undo, before a change.
void remember() {
        @undo = canvas.part(0, 0, canvasWidth, canvasHeight);
}

void changed() {
        if (!modified) {
                modified = true;
                showName();
        }
}

// A line of circles, so a quick stroke stays joined.
void stroke(int x0, int y0, int x1, int y1, int radius, uint color) {
        const int steps = max(abs(x1 - x0), abs(y1 - y0)) / max(radius / 2, 1) + 1;
        for (int s = 1; s <= steps; s++)
                canvas.fillCircle(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, radius, color);
}

void erase(int x0, int y0, int x1, int y1) {
        const int r = max(size, 3);
        const int steps = max(abs(x1 - x0), abs(y1 - y0)) / max(r / 2, 1) + 1;
        for (int s = 1; s <= steps; s++)
                canvas.fillRect(x0 + (x1 - x0) * s / steps - r, y0 + (y1 - y0) * s / steps - r, 2 * r + 1,
                                2 * r + 1, second);
}

// ---- Drawing with the mouse

bool drawing = false;
uint color = Color::Black;    // this stroke's
int startX = 0, startY = 0;   // where it began
int lastX = 0, lastY = 0;     // where it was
Rect shown;                   // a shape's outline drawn so far (the picture under it is undo's)

// A shape from the start to (x, y), the last one's place first put back from undo.
void shape(int x, int y) {
        const int left = min(startX, x), top = min(startY, y);
        const int width = abs(x - startX) + 1, height = abs(y - startY) + 1;
        const int pad = size + 2;
        const Rect now(left - pad, top - pad, width + 2 * pad, height + 2 * pad);
        // What both cover, put back and drawn again.
        const int x0 = min(shown.x, now.x), y0 = min(shown.y, now.y);
        const int x1 = max(shown.x + shown.width, now.x + now.width);
        const int y1 = max(shown.y + shown.height, now.y + now.height);
        canvas.clip(x0, y0, x1 - x0, y1 - y0);
        canvas.image(undo, 0, 0);
        canvas.clip();
        if (tool == Line)
                stroke(startX, startY, x, y, max(size / 2, 1), color);
        else if (tool == Rectangle)
                for (int i = 0; i < max(size / 4, 1); i++) canvas.rect(left + i, top + i, width - 2 * i, height - 2 * i, color);
        else
                for (int i = 0; i < max(size / 4, 1); i++)
                        canvas.ellipse(left + width / 2, top + height / 2, max(width / 2 - i, 0), max(height / 2 - i, 0),
                                       color);
        shown = now;
}

canvas.onPressed(function(int x, int y, int button) {
        if (button != 1 && button != 2) return; // left and right; the middle does nothing
        color = button == 1 ? first : second;
        remember();
        drawing = true;
        startX = lastX = x;
        startY = lastY = y;
        shown = Rect(x, y, 0, 0);
        switch (tool) {
        case Pencil: canvas.plot(x, y, color); break;
        case Brush: stroke(x, y, x, y, max(size / 2, 1), color); break;
        case Eraser: erase(x, y, x, y); break;
        case Fill:
                canvas.floodFill(x, y, color);
                drawing = false;
                break;
        default: shape(x, y); break;
        }
        changed();
});

canvas.onMoved(function(int x, int y, int buttons) {
        if (x >= 0 && y >= 0 && x < canvasWidth && y < canvasHeight)
                status.setField(placeField, x + ", " + y);
        if (!drawing) return;
        switch (tool) {
        case Pencil: canvas.line(lastX, lastY, x, y, color); break;
        case Brush: stroke(lastX, lastY, x, y, max(size / 2, 1), color); break;
        case Eraser: erase(lastX, lastY, x, y); break;
        default: shape(x, y); break;
        }
        lastX = x;
        lastY = y;
});

canvas.onReleased(function(int x, int y, int button) {
        drawing = false;
});

canvas.onResized(function(int width, int height) {
        canvasWidth = width;
        canvasHeight = height;
        status.setField(sizeField, width + " x " + height);
});

// ---- Choosing

toolButtons[Pencil].onClicked(function() { choose(Pencil); });
toolButtons[Brush].onClicked(function() { choose(Brush); });
toolButtons[Line].onClicked(function() { choose(Line); });
toolButtons[Rectangle].onClicked(function() { choose(Rectangle); });
toolButtons[Ellipse].onClicked(function() { choose(Ellipse); });
toolButtons[Fill].onClicked(function() { choose(Fill); });
toolButtons[Eraser].onClicked(function() { choose(Eraser); });

swatches.onPressed(function(int x, int y, int button) {
        const int i = y / cell * columns + x / cell;
        if (x < 0 || y < 0 || x / cell >= columns || i >= int(colors.length())) return;
        if (button == 1) first = colors[i];
        if (button == 2) second = colors[i];
        drawChosen();
});
swatches.onResized(function(int width, int height) { drawSwatches(); });
chosen.onResized(function(int width, int height) { drawChosen(); });

moreButton.onClicked(function() {
        first = chooseColor(first, "First Colour");
        drawChosen();
});

sizeSlider.onChanged(function(int value) {
        size = value;
        sizeLabel.text = "Size " + value;
});

// ---- The file

bool mayDiscard() {
        return !modified || ask("Your changes to " + (path.isEmpty() ? "Untitled" : baseName(path)) +
                                " aren't saved. Discard them?");
}

void newPicture() {
        if (!mayDiscard()) return;
        remember();
        canvas.clear(Color::White);
        path = "";
        modified = false;
        showName();
}

void openPicture() {
        if (!mayDiscard()) return;
        const string chosenPath = chooseFile("Open");
        if (chosenPath.isEmpty()) return;
        Image@ picture;
        try {
                @picture = Image(chosenPath);
        } catch {
                alert("Can't open " + chosenPath + ":\n" + getExceptionInfo());
                return;
        }
        remember();
        canvas.clear(Color::White);
        canvas.image(picture, 0, 0);
        path = chosenPath;
        modified = false;
        showName();
}

bool saveTo(const string &in to) {
        try {
                canvas.save(to);
        } catch {
                alert("Can't save " + to + ":\n" + getExceptionInfo());
                return false;
        }
        path = to;
        modified = false;
        showName();
        return true;
}

void saveAs() {
        const string chosenPath = chooseSaveFile(path.isEmpty() ? "picture.png" : baseName(path), "Save As");
        if (!chosenPath.isEmpty()) saveTo(chosenPath);
}

void save() {
        if (path.isEmpty())
                saveAs();
        else
                saveTo(path);
}

void quit() {
        if (mayDiscard()) app.quit();
}

// Undo swaps the picture with the one kept, so a second undo puts the change back.
void undoChange() {
        if (undo is null) return;
        Image@ now = canvas.part(0, 0, canvasWidth, canvasHeight);
        canvas.clear(Color::White);
        canvas.image(undo, 0, 0);
        @undo = now;
        changed();
}

// ---- Wiring

newItem.shortcut = "Ctrl+N";
openItem.shortcut = "Ctrl+O";
saveItem.shortcut = "Ctrl+S";
quitItem.shortcut = "Ctrl+Q";
undoItem.shortcut = "Ctrl+Z";

newItem.onChosen(newPicture);
openItem.onChosen(openPicture);
saveItem.onChosen(save);
saveAsItem.onChosen(saveAs);
quitItem.onChosen(quit);
undoItem.onChosen(undoChange);
clearItem.onChosen(function() {
        remember();
        canvas.clear(second);
        changed();
});
toolsItem.onChosen(function() {
        tools.show();
        tools.raise();
});
aboutItem.onChosen(function() {
        alert("Paint: a picture drawn on a Canvas in angel.\nThe left button paints in the first colour, "
              "the right in the second.");
});
w.onCloseRequested(quit); // the close box asks first, as Quit does

// The tools' window moved to the right of the picture's (placements are "x y width height").
void placeTools() {
        const string[]@ picture = w.placement.split(" ");
        const string[]@ mine = tools.placement.split(" ");
        if (picture.length() < 4 || mine.length() < 4) return;
        tools.placement = (parseInt(picture[0]) + parseInt(picture[2]) + 8) + " " + picture[1] + " " + mine[2] + " " +
                          mine[3];
}

// The picture's window, then the tools beside it.
choose(Pencil);
sizeSlider.value = size;
showName();
w.show();
tools.show();
placeTools();
w.raise(); // the picture's window active
app.run();
