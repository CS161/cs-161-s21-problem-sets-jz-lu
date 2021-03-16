#include <iostream>
using namespace std;

int say() {
    cout << "hi!\n";
    return 0;
}

int main(int argc, char** argv) {
    cout << "say() comes first:\n";
    if (say() || true) {
        cout << "fired\n";
    }
    cout << "say() comes second:\n";
    if (true || say()) {
        cout << "fired\n";
    }
}
