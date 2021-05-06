#include "u-lib.hh"
#define FRAMELEN        9

// shell 'ping' function, uses IPv4
void process_main(int argc, char** argv) {
    if (argc != 2) {
        console_printf(0xc00, "Usage: ping <IPv4 addr>\n");
        sys_exit(1);
    }

    char* ipaddr = argv[1];
    char ipaddr_cpy[8]; // keep an old copy around to for printing
    strcpy(ipaddr_cpy, ipaddr);

    int off_arr[4] = {0};
    int off_idx = 0;
    size_t sz = strlen(ipaddr);
    for (size_t i = 0; i < sz; ++i) {
        if (ipaddr[i] == '.') {
            off_arr[++off_idx] = i+1;
            ipaddr[i] = '\0';
        }
    }

    if (off_idx != 3) {
        console_printf(0xc00, "Error: Invalid IPv4\n");
        sys_exit(1);
    }

    // Find the 4 chunks of the address (the numbers, separated by the dots).
    uint32_t addr[4];
    for (int i = 0; i < 4; ++i) {
        off_t off = off_arr[i];
        addr[i] = strtol(ipaddr + off);
    } 

    // To anticipate the reader's next question/cry of outrage,
    // I am, in fact, not a psychopath; but I do this in solidarity
    // of my partner and his 4AM brain, a strange and mysterious entity that science
    // has yet to even begin to study.
    uint8_t frame[FRAMELEN];
    frame[0] = 74; //       J
    frame[1] = 111; //      o
    frame[2] = 110; //      n
    frame[3] = 97; //       a
    frame[4] = 116; //      t
    frame[5] = 104; //      h
    frame[6] = 97; //       a
    frame[7] = 110; //      n
    frame[8] = 0;
    console_printf(0xf00, "PING %s (%s) 4(15) bytes of data.\n", ipaddr_cpy, ipaddr_cpy);

    // Wait until ARP table is filled before pinging.
    if (!sys_checkarp()) {
        sys_getarp();
        sys_msleep(2000);
    }

    // Send ICMP ping.
    uint16_t id = rand(129, (1 << 15) - 1); // generate an ID randomly
    for (uint16_t seq = 0; seq < 4; ++seq) {
        uint32_t id_seq = (id << 16) + seq; // packs id and seq into one number [id | seq]
        sys_icmp(addr, frame, FRAMELEN*sizeof(uint8_t), id_seq);
        sys_msleep(500);
    }
    
    sys_exit(0);
}