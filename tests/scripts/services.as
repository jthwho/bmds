// The services (devplan/guiprograms.md §14, step 6a) on the host: processes read, written,
// waited for, signalled and called back in app.run() (with no GUI); dates in named zones (the
// host's database) and POSIX rules; settings and `system` build and answer without the board.

// ---- Processes
Process p("echo one; echo two; echo oops >&2; exit 3");
println(p.readLine() + "," + p.readLine() + " " + p.wait() + " [" + p.readErrors() + "]");
Process cat("cat", {});
cat.writeLine("hello");
cat.write(42);
cat.closeInput();
println("cat: " + cat.readAll() + " " + cat.wait());
Process sleeper("sleep 10");
sleeper.signal();
println("signalled: " + sleeper.wait() + " " + sleeper.running);
try {
        Process none("no-such-program-for-angel", {});
} catch {
        println(getExceptionInfo());
}
Process lines("printf 'a\\nb'");
println(lines.readLine() + lines.readLine() + " " + lines.eof());
// Called back in the loop: each pipe's lines in order (the two pipes' in any), then the end.
string outs = "", errs = "", ending = "";
Process called("echo x; echo y >&2; echo z; exit 5");
called.onOutput(function(line) { outs += line; });
called.onErrors(function(line) { errs += line; });
called.onExit(function(status) { ending = outs + " " + errs + " " + status; });
app.run();
println(ending);

// ---- Dates
DateTime epoch(0, "UTC");
println(epoch.toString() + " " + epoch.weekday + " " + epoch.yearDay);
DateTime moscow(646916400, "Europe/Moscow");
println(moscow.format("%H:%M %Z") + " " + moscow.offset);
DateTime berlin(2026, 7, 1, 12, 30, 0, "Europe/Berlin");
println(berlin.toString() + " " + berlin.seconds + " " + berlin.inZone("UTC").toString());
DateTime normalized(2026, 10, 32, 0, 0, 0, "UTC");
println(normalized.toString());
const string rules = "CET-1CEST,M3.5.0,M10.5.0/3";
println(DateTime(1000000, rules).zone + " " + DateTime(15000000, rules).zone);

// ---- Settings and the system: none on the host
Setting s("wm/palette/desktop");
println("setting: " + s.exists + " [" + s.value + "] " + settings().length());
try {
        s.value = "x";
} catch {
        println(getExceptionInfo());
}
println("cores: " + (system.cores >= 1) + ", load: " + system.cpuLoad);
println("app folder: " + (appDir() == currentDirectory()));
