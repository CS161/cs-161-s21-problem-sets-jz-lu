#include<iostream>
using namespace std;

struct Base {
    Base(int i) {
        cout << "This is the Base speaking: " << i << endl;
    }
    virtual ~Base() {
        cout << "Base is transcending\n";
    }
};

struct Derived:public Base {
    Derived(int i) : Base(i) {
        cout << "This is the Derived speaking: " << i << endl;
    }

    ~Derived() {
        cout << "Derived is transcending\n";
    }
};

int main() {
    Derived(10);
}