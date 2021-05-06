#ifndef CHICKADEE_K_ARP_HH
#define CHICKADEE_K_ARP_HH
#include "k-netdriver.hh"

#define ARP_HRD_ETHERNET    0x0001
#define ARP_OP_REQUEST           1
#define ARP_OP_REPLY             2


// ARP header structs ref: https://pdos.csail.mit.edu/6.828/2017/labs/lab6/, 
// https://github.com/pandax381/xv6-net, https://github.com/mit-pdos/xv6-public.
// Modified according to lwIP https://savannah.nongnu.org/git/?group=lwip.

struct arp_hdr {
    uint16_t hrd;   // hardware type
    uint16_t pro;   // protocol type (default: IP)
    uint8_t hln;    // hardware addr length
    uint8_t pln;    // protocol addr length
    uint16_t op;    // ARP operation type
};

#pragma pack(push, 1) // magic that packs the struct under compiler's nose
struct arp_eth {
    struct arp_hdr hdr;     // ARP header
    uint8_t sha[ADDR_LEN];  // source hardware address
    ipaddr_t spa;           // sender protocol address
    uint8_t tha[ADDR_LEN];  // target hardware address
    ipaddr_t tpa;           // target protocol address
}; // no padding is allowed
#pragma pack(pop)


struct arp_func {
    // Convert IP address to uint32_t.
	static ipaddr_t inet_pton(uint32_t ip_vals[4]);
    // RX ARP handler.
    static void arp_rx (uint8_t *packet, size_t plen, protocol_metadata* meta);
    // Send ARP request to target protocol address.
    static int arp_send_request(ipaddr_t tpa);
    // Send ARP reply to requester addr at dst.
    static int arp_send_reply(const uint8_t *tha, const ipaddr_t *tpa, const uint8_t *dst);
};

struct arp_entry {
    uint8_t ha[ADDR_LEN];      // source hardware address
    ipaddr_t pa = 0xffffffff;  // sender protocol address
};

extern arp_entry arp_table[4];

#endif
