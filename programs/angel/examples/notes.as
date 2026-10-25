#!/rom/bin/angel
// A notepad in a window of its own (the GUI's classes: devplan/guiprograms.md §13): a menu bar
// with shortcuts, a toolbar, the text, and a status bar with the cursor's place. The one-line
// dialogs do the asking (chooseFile(), chooseSaveFile(), ask(), askText(), alert()), and the
// window's icon and the toolbar's are pictures drawn here (angel's Image).

Window w("Notes", 560, 420);
w.minWidth = 300;
w.minHeight = 200;

// ---- Pictures: a notepad for the window, and the toolbar's three

Image@ notepad() {
        Image i(16, 16);
        i.clear(Color::Black);
        i.fillRect(2, 1, 12, 14, Color::White);
        i.rect(2, 1, 12, 14, Color::Gray);
        for (int y = 4; y < 14; y += 3) i.line(4, y, 11, y, Color::Blue);
        i.fillRect(2, 1, 12, 2, Color::Red);
        i.transparent(Color::Black);
        return i;
}
Image@ folder() {
        Image i(16, 16);
        i.clear(Color::Black);
        i.fillRect(1, 3, 6, 2, 0xd8a020);
        i.fillRect(1, 5, 14, 9, 0xf0c040);
        i.rect(1, 5, 14, 9, 0xa07010);
        i.transparent(Color::Black);
        return i;
}
Image@ disk() {
        Image i(16, 16);
        i.clear(Color::Black);
        i.fillRect(1, 1, 14, 14, 0x3060c0);
        i.fillRect(4, 1, 8, 5, Color::White);
        i.fillRect(9, 2, 2, 3, 0x3060c0);
        i.fillRect(3, 9, 10, 6, 0xd0d0d0);
        i.transparent(Color::Black);
        return i;
}
Image@ page() {
        Image i(16, 16);
        i.clear(Color::Black);
        i.fillRect(3, 1, 10, 14, Color::White);
        i.rect(3, 1, 10, 14, Color::Gray);
        i.fillPolygon({9, 1, 13, 5, 9, 5}, 0xc0c0c0);
        i.transparent(Color::Black);
        return i;
}

@w.iconImage = notepad(); // @: the handle is what is set

// ---- The window's parts

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
MenuItem redoItem(editMenu, "&Redo");
editMenu.addSeparator();
MenuItem cutItem(editMenu, "Cu&t");
MenuItem copyItem(editMenu, "&Copy");
MenuItem pasteItem(editMenu, "&Paste");
MenuItem allItem(editMenu, "Select &All");
editMenu.addSeparator();
MenuItem lineItem(editMenu, "&Go to Line...");
Menu helpMenu(bar, "&Help");
MenuItem aboutItem(helpMenu, "&About Notes");

ToolBar tools(w);
ToolButton newButton(tools, "New");
ToolButton openButton(tools, "Open");
ToolButton saveButton(tools, "Save");
@newButton.iconImage = page();
@openButton.iconImage = folder();
@saveButton.iconImage = disk();

TextEdit text(w);
text.stretch = 1;

StatusBar status(w);
int nameField = 0;
int placeField = 0;
nameField = status.addField(0); // the file's name, as wide as there is room for
placeField = status.addField(180);

// ---- The file

string path = ""; // "" until it is opened or saved

string baseName(const string &in p) {
        return p.substr(p.findLast("/") + 1);
}

void showName() {
        const string name = path.isEmpty() ? "Untitled" : baseName(path);
        w.title = (text.modified ? "*" : "") + name + " - Notes";
        status.setField(nameField, path.isEmpty() ? "Not saved yet" : path);
}

// True if the text may be let go: saved, or the user says so.
bool mayDiscard() {
        return !text.modified || ask("Your changes to " + (path.isEmpty() ? "Untitled" : baseName(path)) +
                                     " aren't saved. Discard them?");
}

void newFile() {
        if (!mayDiscard()) return;
        text.text = "";
        text.markSaved();
        path = "";
        showName();
}

void openFile() {
        if (!mayDiscard()) return;
        const string chosen = chooseFile("Open");
        if (chosen.isEmpty()) return;
        try {
                text.text = readFile(chosen);
        } catch {
                alert("Can't open " + chosen + ":\n" + getExceptionInfo());
                return;
        }
        text.markSaved();
        path = chosen;
        showName();
}

bool saveTo(const string &in to) {
        try {
                writeFile(to, text.text);
        } catch {
                alert("Can't save " + to + ":\n" + getExceptionInfo());
                return false;
        }
        text.markSaved();
        path = to;
        showName();
        return true;
}

void saveAs() {
        const string chosen = chooseSaveFile(path.isEmpty() ? "notes.txt" : baseName(path), "Save As");
        if (!chosen.isEmpty()) saveTo(chosen);
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

void goToLine() {
        const string line = askText("Go to line:", "" + (text.cursorLine + 1), "Go to Line");
        if (!line.isEmpty()) text.goToLine(int(parseInt(line)) - 1);
}

// ---- Wiring

newItem.shortcut = "Ctrl+N";
openItem.shortcut = "Ctrl+O";
saveItem.shortcut = "Ctrl+S";
quitItem.shortcut = "Ctrl+Q";
undoItem.shortcut = "Ctrl+Z";
redoItem.shortcut = "Ctrl+Y";
cutItem.shortcut = "Ctrl+X";
copyItem.shortcut = "Ctrl+C";
pasteItem.shortcut = "Ctrl+V";
allItem.shortcut = "Ctrl+A";
lineItem.shortcut = "Ctrl+G";

newItem.onChosen(newFile);
openItem.onChosen(openFile);
saveItem.onChosen(save);
saveAsItem.onChosen(saveAs);
quitItem.onChosen(quit);
undoItem.onChosen(function() { text.undo(); });
redoItem.onChosen(function() { text.redo(); });
cutItem.onChosen(function() { text.cut(); });
copyItem.onChosen(function() { text.copy(); });
pasteItem.onChosen(function() { text.paste(); });
allItem.onChosen(function() { text.selectAll(); });
lineItem.onChosen(goToLine);
aboutItem.onChosen(function() { alert("Notes: a notepad in angel.\nIts dialogs are one line each."); });
newButton.onClicked(newFile);
openButton.onClicked(openFile);
saveButton.onClicked(save);

text.onChanged(showName);
text.onCursorMoved(function(int line, int column) {
        status.setField(placeField, "Line " + (line + 1) + ", column " + (column + 1));
});
w.onCloseRequested(quit); // the close box asks first, as Quit does

showName();
status.setField(placeField, "Line 1, column 1");
w.show();
text.setFocus();
app.run();
