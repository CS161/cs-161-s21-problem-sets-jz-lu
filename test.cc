#include <stdio.h>
#include <string.h>  

int main(void) {
    char c[21] = "/aakash/is/a/donut";
    char* s = c;
    int ndelims = 0;
    int i = 0;
    bool dirend = false;
    if (s[0] == '/') {
        ++s;
    }
    if (s[strlen(s)-1] == '/') {
        s[strlen(s)-1] = '\0';
        dirend = true;
    }
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '/') {
            s[i] = '\0';
            ++ndelims;
        }
    }
    for (; ndelims > 0; --ndelims) {
        s += strlen(s) + 1;
    }
    printf("%s\n", s);

    return 0;
}