#include "k-netdriver.hh"
#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-pci.hh"

e1000state::e1000state(int addr) {
    (void) addr;
}

e1000state* e1000state::find(int addr) { // TODO change name to e1000state
    auto& pci = pcistate::get();
    for (; addr >= 0; addr = pci.next(addr)) {
        if (pci.readw(addr + pci.config_subclass) != 0x0200) { // subclass code for standard NIC
            continue;
        }
        log_printf("[e1000-driver] pci sub-class: 0x%x, vendor: 0x%x, command:0x%x\n", 
            pci.readw(addr + pci.config_subclass), pci.readw(addr + pci.config_vendor), pci.readw(addr + pci.config_command));
        return knew<e1000state>(addr); // find() will only ever be called once
    }
    return nullptr;
}
