#!/rom/bin/angel
// Lesson 4: Loops
//
// A loop runs the same lines again and again. That's how a computer counts to a million
// without getting bored.

// while repeats its { } as long as what's in its brackets is true.
int count = 1;
while (count <= 5) {
        println("Count: " + count);
        count++; // ++ adds one
}

// for does the same in one line: start; keep going while; after each time round.
for (int i = 1; i <= 10; i++) {
        println(i + " times 7 is " + i * 7);
}

// Counting down, with sleep() to wait (in milliseconds: 1000 is a second).
for (int i = 5; i >= 1; i--) {
        println(i + "...");
        sleep(500);
}
println("Lift off!");

// break leaves a loop early.
for (int n = 1; n <= 100; n++) {
        if (n * n > 50) {
                println(n + " is the first number whose square is more than 50.");
                break;
        }
}

// Loops can go inside loops: a little table.
for (int row = 1; row <= 4; row++) {
        string line = "";
        for (int column = 1; column <= 4; column++) line += (row * column) + "\t";
        println(line);
}

// Try this:
//   1. Print the 9 times table.
//   2. Add up all the numbers from 1 to 100 (the answer is 5050).
//   3. Print a triangle of stars: *, then **, then ***, up to 10 stars.
