#!/rom/bin/angel
// Lesson 14: Files
//
// Variables are forgotten when a program ends. Files are kept: a program can write one and read
// it next time. Here, a reaction-time game remembers your best time.

// Where to keep it: a file in your home folder. env("HOME") is that folder's path.
string scoreFile = env("HOME") + "/reaction-best.txt";

// fileExists says whether it's there, readFile gives everything in it as a string.
int best = 0;
if (fileExists(scoreFile)) {
        best = int(parseInt(readFile(scoreFile).trim()));
        println("Your best so far: " + best + " milliseconds.");
} else {
        println("No best time yet: this is your first go.");
}

println("When you see GO!, press Enter as fast as you can.");
input("Press Enter to start... ");
sleep(random(1500, 4000)); // a wait you can't guess
uint64 start = millis();   // milliseconds since the computer started
input("GO! ");
int time = int(millis() - start);
println("You took " + time + " milliseconds.");

if (best == 0 || time < best) {
        println("A new best time!");
        // writeFile replaces what was in the file (or makes it).
        writeFile(scoreFile, "" + time);
}

// appendFile adds to the end, so a file can keep a list: every go, with the time.
appendFile(env("HOME") + "/reaction-log.txt", time + "\n");

// Try this:
//   1. Print every time in the log: readFile it, then split("\n") it into lines.
//   2. Show the average of all the times in the log.
//   3. Keep the best three times, not just the best one.
