// Folders watched, files' types and actions on the board (devplan/guiprograms.md §14, step 6b): a
// folder's changes called back in the loop (the system's fs_watch), merged until taken; the
// types of a program, a text, a folder and a picture; a declaration of actions the system
// accepts, its bindings as text, its sets. Run with `angel tests/board/files.as`; it prints
// "files.as: ok". openFile(), pads and actions on a Screen are tried by hand.

int failures = 0;
void check(bool ok, const string &in what) {
        if (ok) return;
        println("files.as: failed: " + what);
        failures++;
}

// ---- A folder watched
string dir = "/tmp/angel-files-test";
if (!isDirectory(dir)) makeDirectory(dir);
FolderWatch w(dir);
check(w.watching && w.path == dir, "a folder watched");
int[] seen;
int step = 0;
void next() {
        step++;
        if (step == 1) writeFile(dir + "/a.txt", "x");
        if (step == 2) makeDirectory(dir + "/sub");
        if (step == 3) {
                for (int i = 0; i < 20; i++) writeFile(dir + "/many" + i, "x");
                removeFile(dir + "/sub");
        }
        if (step == 4) app.quit();
}
w.onChanged(function(bits) {
        seen.insertLast(bits);
        app.after(50, next);
});
app.after(10, next);
app.after(5000, function() { app.quit(); });
app.run();
check(seen.length() == 3, "three callbacks (" + seen.length() + ")");
if (seen.length() == 3) {
        check(seen[0] == FolderChange::Contents, "a file written: Contents");
        check(seen[1] == FolderChange::Entries, "a folder made: Entries");
        check(seen[2] == (FolderChange::Contents | FolderChange::Entries), "21 changes: one callback");
}
w.stop();
check(!w.watching, "stopped");
bool raised = false;
try {
        FolderWatch none(dir + "/none");
} catch {
        raised = true;
}
check(raised, "a missing folder raises");
raised = false;
try {
        FolderWatch file(dir + "/a.txt");
} catch {
        raised = true;
}
check(raised, "a file raises");

// ---- Types
check(fileType(dir + "/a.txt") == "text/plain", "a text's type: " + fileType(dir + "/a.txt"));
check(fileType(dir) == "inode/directory", "a folder's type: " + fileType(dir));
check(fileType("/rom/bin/angel") == "application/x-executable", "a program's type: " + fileType("/rom/bin/angel"));
check(fileType(dir + "/none") == "", "nothing there: \"\"");
string[] names = listDirectory(dir);
for (uint i = 0; i < names.length(); i++) removeFile(dir + "/" + names[i]);
removeFile(dir);

// ---- Actions
controls.declare("""
[Controls]
Sets=play;menu

[Action left]
Label=Turn left
Sets=play
Keys=Left;A
Pad=Pad.Left;Pad.LeftX-

[Action fire]
Id=7
Label=Fire
Keys=Space
Pad=Pad.South
""");
check(controls.activeSet == "play", "the first set active");
check(controls.keys("left") == "Left; A", "an action's keys: " + controls.keys("left"));
check(controls.pad("fire") == "Pad.South", "an action's pad: " + controls.pad("fire"));
controls.activeSet = "menu";
controls.activeSet = "";
check(controls.activeSet == "", "no set");
raised = false;
try {
        controls.activeSet = "none";
} catch {
        raised = true;
}
check(raised, "a set not declared raises");
raised = false;
try {
        controls.declare("[Action a]\nLabel=No keys\n");
} catch {
        raised = true;
}
check(raised, "an action without keys refused");

if (failures == 0) println("files.as: ok");
