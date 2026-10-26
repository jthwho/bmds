#!/rom/bin/angel
// Lesson 17: Two windows
//
// A program can have as many windows as it likes. They're variables like any other, so the
// callbacks of one can change another. Here: a to-do list, and a second window for adding to
// it, which can be opened and closed while the list stays.

// ---- The list window
Window listWindow("To do", 300, 320);
listWindow.margin = 8;
listWindow.spacing = 6;
ListView tasks(listWindow); // a list of lines, one of which can be the current one
tasks.stretch = 1;
Label countLabel(listWindow, "");
Box row(listWindow, "across");
Button addButton(row, "&Add...");
Button doneButton(row, "&Done");

// ---- The adding window: made now, shown only when it's wanted
Window addWindow("Add a task", 280, 110);
addWindow.margin = 8;
addWindow.spacing = 6;
// It belongs to the list: it stays above it. The @ says that what is set is which window it
// is (a handle), not a copy of one.
@addWindow.owner = listWindow;
Label prompt(addWindow, "What is there to do?");
LineEdit taskBox(addWindow);
Box addRow(addWindow, "across");
Button addTaskButton(addRow, "Add &it");
Button closeButton(addRow, "&Close");

// What both windows share: a function showing how many tasks there are.
void showCount() {
        int n = tasks.items.length();
        countLabel.text = n == 0 ? "Nothing to do!" : n + (n == 1 ? " task" : " tasks");
}

// The list window's buttons open the other window, or tick off the current task.
addButton.onClicked(function() {
        addWindow.show();
        taskBox.setFocus();
});
doneButton.onClicked(function() {
        if (tasks.current < 0) return; // nothing chosen: -1
        tasks.removeItem(tasks.current);
        showCount();
});
// A double click (or Enter) on a task: activated, with its number.
tasks.onActivated(function(index) {
        alert("Task " + (index + 1) + ": " + tasks.items[index]);
});

// The adding window's callbacks change the list window: this is how windows talk.
void addTask() {
        string task = taskBox.text.trim();
        if (task.length() == 0) return;
        tasks.addItem(task);
        taskBox.text = "";
        showCount();
}
addTaskButton.onClicked(addTask);
taskBox.onAccepted(function(text) { addTask(); });
closeButton.onClicked(function() { addWindow.close(); });

// Closing the list window ends the program, even if the other window is open.
listWindow.onClosed(function() { app.quit(); });

tasks.addItem("Read lesson 17");
showCount();
listWindow.show();
app.run();

// Try this:
//   1. Keep the list in a file (dataDir() + "/todo.txt"): load it at the start, save it when
//      the list window closes. split("\n") turns the file back into lines.
//   2. Add an "Edit..." button that puts the current task into the adding window's box.
//   3. Put a third window with a big label showing only the current task, updated by the list's
//      onCurrentChanged callback.
