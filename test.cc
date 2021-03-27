#include <iostream>
using namespace std;

int say() {
    cout << "hi!\n";
    return -1;
}

int main(int argc, char** argv) {
    if (int r = say() < 0) {
        cout << "worked!\n";
    }
}
