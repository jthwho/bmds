// Files and directories, in a temporary directory.
string dir = env("TEST_TMP") + "/work";
makeDirectory(dir);
println(isDirectory(dir));
File@ f = File(dir + "/notes.txt", "w");
f.writeLine("first line");
f.write(42);
f.writeLine("");
int[] pair = {1, 2};
f.writeLine(pair);
f.close();
println(f.isOpen());
File@ r = File(dir + "/notes.txt");
while (!r.eof()) println("> " + r.readLine());
r.close();
appendFile(dir + "/notes.txt", "added\n");
println(readFile(dir + "/notes.txt").split("\n").length());
writeFile(dir + "/b.txt", "bee");
println(listDirectory(dir));
renameFile(dir + "/b.txt", dir + "/c.txt");
println(fileExists(dir + "/b.txt"));
println(readFile(dir + "/c.txt"));
removeFile(dir + "/c.txt");
println(listDirectory(dir));
try {
        File@ missing = File(dir + "/nope.txt");
} catch {
        println(getExceptionInfo().replace(dir, "DIR"));
}
try {
        File@ bad = File(dir + "/x", "rw");
} catch {
        println(getExceptionInfo());
}
File@ w = File(dir + "/w.txt", "w");
try { w.readLine(); } catch { println(getExceptionInfo()); }
string here = currentDirectory();
changeDirectory(dir);
println(fileExists("notes.txt"));
changeDirectory(here);
