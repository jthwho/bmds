#!/rom/bin/angel
// Lesson 16: A small application
//
// A notepad that keeps what you write: a window with a menu, a big box of text, and a status
// bar at the bottom. It saves to a file of its own, so your notes are there next time, and it
// asks before closing if you haven't saved.

// dataDir() is a folder that belongs to this program, made the first time it's asked for: the
// place for what it keeps: for this lesson, .local/share/16-an-application in your home folder.
// (Lesson 14 used your home folder itself; this way programs don't mix up each other's files.)
string notesFile = dataDir() + "/notes.txt";

Window w("My notes", 480, 360);
w.minWidth = 240;
w.minHeight = 160;

// A menu bar, its menus, and their items. shortcut is the key that chooses an item from
// anywhere in the window.
MenuBar bar(w);
Menu fileMenu(bar, "&File");
MenuItem saveItem(fileMenu, "&Save");
saveItem.shortcut = "Ctrl+S";
MenuItem clearItem(fileMenu, "C&lear");
fileMenu.addSeparator(); // a line between items
MenuItem quitItem(fileMenu, "&Quit");
quitItem.shortcut = "Ctrl+Q";

// A TextEdit holds many lines. stretch = 1 makes it take the room the window has spare.
TextEdit text(w);
text.stretch = 1;

// A status bar: fields of text along the bottom. addField(0) makes one as wide as there's room
// for, and gives its number.
StatusBar status(w);
int field = 0;
field = status.addField(0);

// Load what was saved last time, if anything was. markSaved() says "this is what's in the
// file": from then on, text.modified says whether it has changed since.
if (fileExists(notesFile)) text.text = readFile(notesFile);
text.markSaved();

void showState() {
        status.setField(field, text.modified ? "Changed: Ctrl+S saves" : "Saved in " + notesFile);
        w.title = (text.modified ? "*" : "") + "My notes"; // a star in the title, as editors do
}

void save() {
        writeFile(notesFile, text.text);
        text.markSaved();
        showState();
}

// Before the window closes: if there are changes, ask() puts a question with Yes and No.
void quit() {
        if (text.modified && ask("Save your notes before closing?")) save();
        w.close();
}

saveItem.onChosen(save);
clearItem.onChosen(function() { text.text = ""; });
quitItem.onChosen(quit);
text.onChanged(showState); // called at every change to the text

// The window's close box would close it at once. onCloseRequested gives it to quit() instead,
// which asks first and then closes the window itself.
w.onCloseRequested(quit);

showState();
w.show();
text.setFocus();
app.run();

// Try this:
//   1. Add a status field showing how many characters there are (text.text.length()).
//   2. Save on its own every minute: app.every(60000, save) calls save once a minute.
//   3. Add an Edit menu with "Insert the date": text.insert(DateTime().toString()) puts it
//      where the cursor is.
