#!/rom/bin/angel
// Lesson 6: Functions
//
// A function is a few lines with a name. Write them once, then use the name as often as you
// like. println is a function someone wrote for you; now you write your own.

// This one takes nothing and gives back nothing (void): it just does something.
void line() {
        println("--------------------");
}

// This one takes a value: inside, "name" is whatever was given.
void greet(string name) {
        println("Hello, " + name + "!");
}

// This one gives back a value with return. Its type (float) comes first.
float circleArea(float radius) {
        return PI * radius * radius;
}

// A function can use if, loops, and other functions.
bool isPrime(int n) {
        if (n < 2) return false;
        for (int d = 2; d * d <= n; d++)
                if (n % d == 0) return false; // it divides: not prime
        return true;
}

line();
greet("Ada");
greet("Grace");
line();
println("A circle of radius 2 has an area of " + circleArea(2));
println("And one of radius 10: " + circleArea(10));
line();
string primes = "";
for (int n = 1; n <= 50; n++)
        if (isPrime(n)) primes += n + " ";
println("The primes up to 50: " + primes);
line();

// Try this:
//   1. Write a function square(int n) that returns n * n, and print square(12).
//   2. Give greet a second parameter, the number of times to say hello.
//   3. Write isEven(int n). (Hint: n % 2 is 0 for an even number.)
