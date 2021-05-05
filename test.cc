#include <stdio.h>
#include <string.h>  

int main(void) {
    while (int r = 3 == 3) {
        printf("%d\n", r);
    }

    return 0;
}

while (true) {
    uint32_t tail = (reg_read(E1000_RDT)+1) % RX_RING_SIZE; // tx tail index
    rx_desc* desc = &rxring_[tail];

    // Break if status is done (DD).
    if (!(desc->status & E1000_RXD_STAT_DD)) {
        break;
    } 
    if (!(desc->errors || !(desc->status & E1000_RXD_STAT_EOP) 
        || desc->length < short_package_sz)) {
        rx_ethernet_formatter(meta_, (uint8_t*)desc->addr, desc->length);
    }
    desc->status = (uint16_t)(0);
    // here we clear the descriptor status
    reg_write(E1000_RDT, tail);
    // we then update the tail
}