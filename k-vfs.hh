#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-devices.hh"
#include "k-wait.hh"

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    kb_c_vnode();

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile* mf_; // in-memory file, lock defined in here

    memfile_vnode(int mode, memfile* mf);

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};



#endif