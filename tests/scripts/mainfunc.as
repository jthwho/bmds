// A script of declarations only runs its main(); int main()'s value is the exit status.
// exit: 3
int helper() { return 3; }
int main() {
        println("in main");
        return helper();
}
