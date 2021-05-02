#ifndef CHICKADEE_K_NETDRIVER_HH
#define CHICKADEE_K_NETDRIVER_HH
#include "k-wait.hh"

struct e1000state {

    static e1000state* find(int pci_addr=0);
    e1000state(int pci_addr);
};

#endif