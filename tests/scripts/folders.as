// Folders watched, files' types and actions (devplan/guiprograms.md §14, step 6b) on the host: a
// FolderWatch's changes called back in app.run() (inotify standing in for the system's watch),
// the bits merged until the loop takes them; fileType() can't tell here; controls.declare()'s
// names and Ids checked (nothing bound on the host).

string dir = env("TEST_TMP") + "/watched";
makeDirectory(dir);
FolderWatch w(dir);
println(w.watching + " " + (w.path == dir));

// Each change in turn: written, an entry made and removed, both at once.
int[] seen;
int step = 0;
void next() {
        step++;
        if (step == 1) writeFile(dir + "/a.txt", "x");
        if (step == 2) makeDirectory(dir + "/sub");
        if (step == 3) removeFile(dir + "/sub");
        if (step == 4) {
                writeFile(dir + "/b.txt", "y");
                renameFile(dir + "/b.txt", dir + "/c.txt");
        }
        if (step == 5) app.quit();
}
w.onChanged(function(bits) {
        seen.insertLast(bits);
        app.after(30, next); // the next change after this one's are all in
});
app.after(10, next);
app.after(5000, function() { app.quit(); });
app.run();
string told = "";
for (uint i = 0; i < seen.length(); i++) told += " " + seen[i];
println("told:" + told);
println("Entries " + FolderChange::Entries + ", Contents " + FolderChange::Contents + ", Mounts "
        + FolderChange::Mounts);

// Stopped: no more callbacks; a callback can't be given.
w.stop();
println("stopped: " + w.watching);
try {
        w.onChanged(function(bits) {});
} catch {
        println("a callback refused: " + getExceptionInfo().contains("has stopped"));
}
try {
        FolderWatch none(dir + "/none");
} catch {
        println("missing: " + (getExceptionInfo().contains("No such file")));
}
try {
        FolderWatch file(dir + "/a.txt");
} catch {
        println("a file: " + (getExceptionInfo().contains("Not a directory")));
}
removeFile(dir + "/a.txt");
removeFile(dir + "/c.txt");
removeFile(dir);

// Types: the system tells them; the host can't.
println("type: '" + fileType("folders.as") + "'");

// Actions: names and Ids checked, Ids given to those without.
controls.declare("""
[Controls]
Sets=play;menu

[Action left]
Keys=Left

[Action fire]
Id=1
Keys=Space
Pad=Pad.South
""");
println("set: " + controls.activeSet + ", keys: '" + controls.keys("fire") + "'");
controls.activeSet = "menu";
println("set: " + controls.activeSet);
string[] bad = {"[Action Fire]\nKeys=X\n", "[Action a]\nKeys=X\n[Action a]\nKeys=Y\n",
                "[Action a]\nId=1\nKeys=X\n[Action b]\nId=1\nKeys=Y\n", "[Action a]\nId=70000\nKeys=X\n",
                "[Action a\nKeys=X\n"};
for (uint i = 0; i < bad.length(); i++) {
        try {
                controls.declare(bad[i]);
        } catch {
                println(getExceptionInfo());
        }
}
try {
        controls.keys("jump");
} catch {
        println(getExceptionInfo());
}
println("PadDown " + PadDown + ", ActionMove " + ActionMove);
