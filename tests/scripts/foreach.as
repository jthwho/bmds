// foreach over value types (strings): each item destroyed once, break and continue and a
// return leaving cleanly, nested loops, the index as a second item (the foreach patch,
// thirdparty/angelscript/README.md).
string[] f = {"a", "bb", "ccc"};
foreach (string s : f) println(s);
foreach (string s : f) {
        if (s == "bb") continue;
        println("continue " + s);
}
foreach (string s : f) {
        if (s == "bb") break;
        println("break " + s);
}
string last;
foreach (const string s : f) last = s;
println(last);
string[][] nested = {{"x", "y"}, {"z"}};
foreach (string[] row : nested) foreach (string s : row) print(s);
println();
string first(const string[] &in list) {
        foreach (string s : list)
                if (s.length() > 1) return s;
        return "";
}
println(first(f));
foreach (string s, uint i : f) print(i + s + " ");
println();
for (int round = 0; round < 1000; round++)
        foreach (string s : f) last = s + round;
println(last);
