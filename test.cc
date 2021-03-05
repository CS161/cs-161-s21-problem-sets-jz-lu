#include <unistd.h>
#include <stdio.h>

int main() {
    write(1, "Hello\n", 6);
    close(1);
    write(0, "Hello again\n", 12);
    dup2(0, 1);
    write(1, "Hi\n", 3);
}