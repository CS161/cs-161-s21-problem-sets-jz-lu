#include<iostream>
using namespace std;

struct Base {
    Base(int i) {
        cout << "This is the Base speaking: " << i << endl;
    }
    ~Base() {
        cout << "Base is transcending\n";
    }
};

struct Derived:public Base {
    Derived(int i) : Base(i) {
        cout << "This is the Derived speaking: " << i << endl;
    }
};

int main() {
    void* a[10];
    cout << 10*sizeof(void*) << endl;
}