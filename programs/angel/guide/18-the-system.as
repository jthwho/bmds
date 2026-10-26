#!/rom/bin/angel
// Lesson 18: The computer around your program
//
// A program can ask the computer what time it is, keep a setting of its own, run other
// programs, and speak. Here, in one window: a clock, a greeting you can change and that stays
// changed, a box to run a command in, and a button that says the time.

Window w("Around the computer", 440, 360);
w.margin = 8;
w.spacing = 6;

// ---- A clock: app.every(milliseconds, callback) calls it again and again, while the window
// waits for clicks. DateTime() is now; format() writes it as you ask (%H hours, %M minutes,
// %S seconds, %A the day's name).
Label clock(w, "");
void tick() {
        clock.text = DateTime().format("%A %H:%M:%S") + "   Free memory: " + (system.memoryFree / 1048576)
                     + " MB";
}
tick();
app.every(1000, tick);

// ---- A setting of the program's own. The system keeps settings (the ones in the Settings
// window are some); a program can declare its own: a name, its kind, its default, and what it's
// for. save() keeps it for next time, even after the computer restarts.
Setting greetingSetting("guide/greeting", "string", "Hello", "Lesson 18's greeting");
Box greetRow(w, "across");
Label greetLabel(greetRow, "Greeting:");
LineEdit greetBox(greetRow);
greetBox.stretch = 1;
greetBox.text = greetingSetting.value;
Label greeting(w, greetingSetting.value + ", " + system.userName + "!");
greetBox.onAccepted(function(text) {
        greetingSetting.value = text;
        greetingSetting.save();
        greeting.text = text + ", " + system.userName + "!";
});

// ---- Running a command, as you would in the shell. A Process runs it beside this program;
// its output comes back a line at a time to onOutput, and onExit gets its exit status (0: it
// went well).
Box runRow(w, "across");
LineEdit commandBox(runRow);
commandBox.stretch = 1;
commandBox.text = "ls /";
Button runButton(runRow, "&Run");
TextEdit results(w); // (not "output": that's a function's name already)
results.readOnly = true;
results.stretch = 1;
string shown = "";
void run() {
        shown = "";
        Process p(commandBox.text);
        p.onOutput(function(line) {
                shown += line + "\n";
                results.text = shown;
        });
        p.onExit(function(status) {
                shown += "(finished: " + status + ")\n";
                results.text = shown;
        });
}
runButton.onClicked(run);
commandBox.onAccepted(function(text) { run(); });

// ---- Speaking: a Voice says text in the background (speak), so the window carries on.
Voice voice;
Button timeButton(w, "Say the &time");
timeButton.onClicked(function() { voice.speak("It's " + DateTime().format("%H:%M") + "."); });

w.show();
app.run();

// Try this:
//   1. Show the time somewhere else: DateTime().inZone("Asia/Tokyo") is the same moment in
//      Tokyo.
//   2. Make the voice slower or higher: voice.speed = 70, voice.pitch = 130 (percent).
//   3. Play a sound file of yours (a .wav, .mp3, .ogg or .flac) in the background:
//      AudioPlayer(chooseFile()).play(). There's pause(), stop() and position too; Help >
//      Examples > Player shows more.
