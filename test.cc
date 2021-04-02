#include <stdio.h>
#include <string.h>  

namespace chkfs {
    const int maxnamelen = 123;
}

char* strapp(char* s1, char* s2, char* buf, bool dir=true) {
    if (strlen(s1) + strlen(s2) > chkfs::maxnamelen-2) {
        return nullptr;
    }
    strcpy(buf, s1);
    if (s2[0] == '/') {
        ++s2;
    }
    memcpy((void*) (buf+strlen(s1)), (void*) s2, strlen(s2)+1);
    int blen = strlen(buf);
    if (dir && buf[blen-1] != '/') {
        buf[blen+1] = '\0';
        buf[blen] = '/';
    }
    return buf;
}

int main(void) {
    char c2[24] = "/donut.txt";
    char c1[15] = "/aakash/is/a/";
    char s[strlen(c1)+strlen(c2)+1];
    strapp(c1, c2, s, false);
    printf("WORD: %s\n", s);

    return 0;
}