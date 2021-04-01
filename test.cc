#include <stdio.h>
#include <string.h>  

char* path_find_last(char* s) {
    if (!strchr(s, '/')) {
        printf("NO DELIMS\n");
        return s;
    }
    if (s[strlen(s)-1] == '/') {
        s[strlen(s)-1] = '\0';
    }
    int last_delim_index = 0;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '/') {
            printf("last delim index updated to %d\n", i);
            last_delim_index = i;
            printf("UPDATED WORD: %s\n", s + last_delim_index + 1);
        }
    }
    return s + last_delim_index + 1;
}

int main(void) {
    char c[24] = "donut.txt";
    char* s = path_find_last(c);
    printf("WORD: %s\n", s);

    return 0;
}