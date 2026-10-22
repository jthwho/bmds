// What print() shows for each kind of value.
println("text");
println(42);
println(-7);
println(uint(4000000000));
println(int64(-9000000000));
println(1.5);
println(0.1);
println(1.0 / 3);
println(0.1d);
println(1.0d / 3);
println(true);
println(false);
println('A');
int[] numbers = {3, 1, 2};
println(numbers);
string[] words = {"a", "b c"};
println(words);
array<int[]> nested = {{1, 2}, {3}};
println(nested);
int[]@ none;
println(@none);
enum Fruit { Apple, Pear }
println(Pear);
println(Fruit(7));
class Plain { }
println(Plain());
class Named { string toString() const { return "I am Named"; } }
println(Named());
print("no newline");
println();
