#!/rom/bin/angel
// The system at a glance (the services: devplan/guiprograms.md §14): its names, memory, CPU
// load and network, refreshed each second; a clock in a time zone kept as the example's own
// setting (declared, saved when changed); and a command run with its output coming in line by
// line (a Process's callbacks in the loop), as a terminal would show it.

Window w("System", 460, 420);
w.minWidth = 320;
w.minHeight = 300;
w.margin = 8;
w.spacing = 6;

Form form(w);
FormRow hostRow(form, "Host:");
Label host(hostRow, system.hostName + " (" + system.board + ", BMDS " + system.version + ")");
FormRow cpuRow(form, "CPU:");
Label cpu(cpuRow, system.cores + " cores at " + system.cpuMhz + " MHz");
FormRow loadRow(form, "Load:");
Label load(loadRow, "");
FormRow memoryRow(form, "Memory:");
Label memory(memoryRow, "");
FormRow ownRow(form, "This script:");
Label own(ownRow, "");
FormRow networkRow(form, "Network:");
Label network(networkRow, "");
FormRow clockRow(form, "Time:");
Label clock(clockRow, "");
FormRow zoneRow(form, "Zone:");
LineEdit zoneEdit(zoneRow);

// The clock's zone: the example's own setting, its saved value back at each start.
Setting zone("system-example/zone", "string", "", "The time zone the System example's clock shows");
zoneEdit.text = zone.value;
zoneEdit.onAccepted(function(text) {
        zone.value = text;
        zone.save();
        refresh();
});

string megabytes(int64 bytes) {
        return formatFloat(double(bytes) / 1048576, 1) + " MB";
}

void showNetwork() {
        const string address = localAddress();
        network.text = system.online ? "online, " + address : "offline";
}

void refresh() {
        const float share = system.cpuLoad;
        load.text = share < 0 ? "not measured (sysmon/enable)" : formatFloat(share * 100, 1) + "% busy";
        memory.text = megabytes(system.memoryFree) + " free of " + megabytes(system.memoryTotal);
        own.text = megabytes(system.programMemory);
        DateTime now = DateTime().inZone(zone.value);
        clock.text = now.format("%A %d %B %Y, %H:%M:%S %Z");
}

// A command and what it writes.
Box commandRow(w, "across");
LineEdit command(commandRow);
command.stretch = 1;
command.text = "ls /";
Button runButton(commandRow, "&Run");
TextEdit shown(w);
shown.readOnly = true;
shown.stretch = 1;
StatusBar status(w);
int field = 0;
field = status.addField(0); // as wide as there's room for

Process@ running = null;
string written = "";

void add(const string &in line) {
        written += line + "\n";
        if (written.length() > 20000) written = written.substr(written.length() - 20000);
        shown.text = written;
}

void run() {
        if (running !is null && running.running) running.signal();
        written = "";
        shown.text = "";
        status.setField(field, "Running " + command.text);
        @running = Process(command.text);
        running.closeInput();
        running.onOutput(add);
        running.onErrors(add);
        running.onExit(function(code) { status.setField(field, "Exit status " + code); });
}
runButton.onClicked(run);
command.onAccepted(function(text) { run(); });

showNetwork();
system.onNetworkChanged(showNetwork);
refresh();
app.every(1000, refresh);
w.show();
app.run();
