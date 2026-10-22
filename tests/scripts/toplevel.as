// Statements outside functions run from top to bottom; globals declared among them are
// initialized where they stand; functions and types may come after their use.
// args: first second
// stdin: Ada\n
println("start");
println(count);
print("Name? ");
string name = input();
println("Hello, " + name);
println(greeting());
string greeting() { return "greeting sees '" + name + "' and count " + count; }
int count = 3;
println(greeting());
const int SIZE = 4;
int[] grid(SIZE);
println(grid.length());
Thing@ thing = Thing(5);
println(thing.value);
class Thing { int value; Thing(int v) { value = v; } }
println(args);
println(args.length());
if (args.length() > 1) {
        int local = 2;
        println("local " + local);
}
for (int i = 0; i < 2; i++) println(i);
namespace Tools { int triple(int x) { return x * 3; } }
println(Tools::triple(3));
return;
