// angel's service bindings on the board (devplan/guiprograms.md §14, step 6a): a setting read, one
// of the script's own declared, set, watched and reverted (nothing saved); processes read,
// written, waited for, signalled and called back in the loop, many in turn (descriptors given
// back); the system's names, memory, load and network; dates in zones with their history; the
// script's folders. Run with `angel tests/board/services.as`; it prints "services.as: ok".

int failures = 0;
void check(bool ok, const string &in what) {
        if (ok) return;
        println("services.as: failed: " + what);
        failures++;
}

// ---- Settings
Setting tzdata("clock/tzdata");
check(tzdata.exists && tzdata.type == "path" && tzdata.value.length() > 0, "a system setting read");
check(tzdata.help.length() > 0 && tzdata.defaultValue.length() > 0, "its help and default");
Setting missing("angel-test/none");
check(!missing.exists && missing.value == "", "a missing setting");
bool raised = false;
try {
        missing.value = "x";
} catch {
        raised = true;
}
check(raised, "setting a missing setting raises");
check(settings("clock").length() >= 2, "settings() under a prefix");

Setting size("angel-test/size", "int 1 100", "10", "A test's number");
check(size.exists && size.type == "int" && size.value == "10" && size.isDefault, "a setting declared");
size.value = "42";
check(size.value == "42" && !size.isDefault && size.changed && !size.isSaved, "a declared setting set");
raised = false;
try {
        size.value = "500";
} catch {
        raised = true;
}
check(raised && size.value == "42", "a value outside its limits raises");
Setting mood("angel-test/mood", "calm|busy|sleepy", "calm");
check(mood.type == "enum" && mood.choices == "calm|busy|sleepy", "an enum declared");
raised = false;
try {
        mood.value = "angry";
} catch {
        raised = true;
}
check(raised, "a word not of the enum raises");
Setting again("angel-test/size", "int 1 100", "10");
check(again.value == "42", "declared again: the same setting");

// Watched: a change made from a timer comes back in the loop.
string heard = "";
size.onChanged(function(value) {
        heard = value;
        app.quit();
});
app.after(10, function() { size.value = "7"; });
app.after(3000, function() { app.quit(); });
app.run();
check(heard == "7", "a setting's change called back");
size.onChanged(null);
size.revert();
check(size.value == "10" && size.isDefault, "reverted to its default");
size.value = "3";
size.reset();
check(size.value == "10", "reset to its default");

// ---- Processes
Process echo("echo one; echo two >&2; exit 4");
check(echo.readLine() == "one", "a command's output line");
check(echo.wait() == 4 && echo.exitStatus == 4 && !echo.running, "its exit status");
check(echo.readErrors() == "two\n", "its errors");
check(echo.readLine() == "" && echo.eof(), "its output ended");

Process cat("cat", {});
cat.writeLine("hello");
cat.write(12);
cat.closeInput();
check(cat.readAll() == "hello\n12" && cat.wait() == 0, "a program written to and read");

Process echoed("echo", {"a b", "c"});
check(echoed.readAll() == "a b c\n", "a program's arguments, no shell between");

raised = false;
try {
        Process none("no-such-program-x", {});
} catch {
        raised = true;
}
check(raised, "a missing program raises");

Process sleeper("sleep 10");
check(sleeper.running && sleeper.pid > 0, "a process running");
sleeper.signal();
check(sleeper.wait() == 128 + 15, "a process signalled");
raised = false;
try {
        sleeper.write("late");
} catch {
        raised = true;
}
check(raised, "writing to an ended process raises");

int runs = 0;
for (int i = 0; i < 40; ++i) {
        Process p("true");
        if (p.wait() == 0) runs++;
}
check(runs == 40, "40 processes in turn (descriptors given back)");

string lines = "";
int status = -1;
Process counter("for i in 1 2 3; do echo $i; done; exit 2");
counter.onOutput(function(line) { lines += line; });
counter.onExit(function(code) {
        status = code;
        app.quit();
});
app.after(5000, function() { app.quit(); });
app.run();
check(lines == "123" && status == 2, "a process's lines and end called back in the loop");

// ---- The system
check(system.hostName.length() > 0 && system.userName.length() > 0, "the host's and user's names");
check(system.board.length() > 0 && system.version.length() > 0, "the board and the version");
check(system.cores >= 1 && system.cpuMhz > 0, "the cores");
check(system.memoryTotal > 0 && system.memoryFree > 0 && system.memoryFree <= system.memoryTotal,
      "the memory");
check(system.programMemory > 0, "the script's own memory");
float load = system.cpuLoad;
sleep(200);
load = system.cpuLoad;
check(load == -1 || (load >= 0 && load <= 1), "the CPU's load");
check(system.online, "the network is there");
system.onNetworkChanged(function() { println("the network changed"); });
system.onNetworkChanged(null);

// ---- Dates
DateTime epoch(0, "UTC");
check(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1 && epoch.weekday == 4, "the epoch");
check(epoch.toString() == "1970-01-01 00:00:00 UTC", "a date as text");
DateTime moscow(646916400, "Europe/Moscow");
check(moscow.hour == 15 && moscow.zone == "MSD", "a zone's history (1990's summer time)");
DateTime berlin(2026, 7, 1, 12, 0, 0, "Europe/Berlin");
check(berlin.offset == 7200 && berlin.format("%H:%M %Z") == "12:00 CEST", "a date made in a zone");
check(berlin.inZone("UTC").hour == 10, "the same moment in another zone");
const string rules = "CET-1CEST,M3.5.0,M10.5.0/3";
check(DateTime(1000000, rules).zone == "CET" && DateTime(15000000, rules).zone == "CEST", "POSIX rules");
check(DateTime().year >= 2026, "now");

// ---- Folders
check(appDir().length() > 0 && isDirectory(appDir()), "the script's folder");
string data = dataDir();
check(data.length() > 0 && isDirectory(data), "its data folder made");
Process("rmdir", {data}).wait(); // not left behind

if (failures == 0) println("services.as: ok");
