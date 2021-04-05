#include "u-lib.hh"

// Test creating and removing subdirectories
void process_main() {
    printf("Starting testwritefs9 (assuming clean file system)...\n");

    // read and write to file
    printf("%s:%d: creating a directory ...\n", __FILE__, __LINE__);

    int ret = sys_mkdir("/jonathan/");
    assert_ge(ret, 0);

    printf("%s:%d: changing directories ...\n", __FILE__, __LINE__);

    ret = sys_cd("/jonathan/");
    assert_eq(ret, 0);

    printf("%s:%d: checking working directory with pwd ...\n", __FILE__, __LINE__);;

    char buf[200];
    memset(buf, 0, sizeof(buf));

    sys_pwd(buf);
    assert_memeq(buf, "/jonathan", 9);

    printf("%s:%d: changing back to root ...\n", __FILE__, __LINE__);

    char root[2] = "/";
    ret = sys_cd(root);
    assert_eq(ret, 0);

    memset(buf, 0, sizeof(buf));

    ret = sys_pwd(buf);
    assert_ge(ret, 0);

    printf("%s:%d: deleting jonathan's folder :-D ...\n", __FILE__, __LINE__);
    // this message sponsored by the "Aakash is cool" group
    // this message condemned by the "Jonathan is cool" group

    ret = sys_rm("/jonathan/");
    assert_eq(ret, 0);

    ret = sys_cd("/jonathan/");
    assert_eq(ret, E_NOENT);

    console_printf(0xA00, "testwritefs9 succeeded.\n");

    sys_exit(0);
}