// An exception: where, and the calls that led there; exit status 1. A catch block gets one.
// exit: 1
try {
        throw("handled");
} catch {
        println("caught: " + getExceptionInfo());
}
int divide(int a, int b) { return a / b; }
int outer(int x) { return divide(10, x); }
println("before");
println(outer(0));
println("after");
