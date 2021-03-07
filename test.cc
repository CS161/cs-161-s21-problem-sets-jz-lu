#include<iostream>
using namespace std;

struct Base {
    Base(int i) {
        cout << "This is the Base speaking: " << i << endl;
    }
};

struct Derived:public Base {
    Derived(int i) : Base(i) {
        cout << "This is the Derived speaking: " << i << endl;
    }
};

int main() {
    Derived(10);
}