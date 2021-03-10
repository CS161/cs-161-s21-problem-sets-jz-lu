#include<iostream>
using namespace std;

struct Base {
    int n = 0;
    Base(int i) {
        n = i;
        cout << "This is the Base speaking: " << i << endl;
    }
    virtual ~Base() {
        cout << "Base is transcending\n";
    }

    virtual int pie() {
        return 3*n;
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

struct Sibling:public Base {
    Sibling(int i) : Base(i) {
        cout << "This is the Sibling speaking: " << i << endl;
    }

    ~Sibling() {
        cout << "Sibling is transcending\n";
    }

    int pie() {
        // cout << "HI, n = " << n << "\n";
        return 5*n;
    }
};

int main() {
    Derived d = Derived(10);
    cout << d.pie() << endl;
    Sibling s = Sibling(10);
    cout << s.pie() << endl;
}