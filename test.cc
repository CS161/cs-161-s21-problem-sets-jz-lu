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
    if (true || false && true) {
        printf("1\n");
    }
    if (true || false && false) {
        printf("2\n");
    }
    if (true || true && false) {
        printf("3\n");
    }
    if (false || true && false) {
        printf("4\n");
    }

    return 0;
}