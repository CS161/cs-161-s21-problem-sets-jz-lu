#ifndef CHICKADEE_K_IP_HH
#define CHICKADEE_K_IP_HH
#include "types.h"
#include "k-netdriver.hh"
#include "k-arp.hh"

// From the xv6 MIT pandax implementation and LWIP implementation
// Ref: ip.c @ https://savannah.nongnu.org/git/?group=lwip
// and https://github.com/pandax381/xv6-net/

// START
#define IP_HDR_SIZE_MIN 20
#define IP_HDR_SIZE_MAX 60
#define IP_PAYLOAD_SIZE_MAX (65535 - IP_HDR_SIZE_MIN)

#define IP_ADDR_LEN 4
#define IP_ADDR_STR_LEN 16 /* "ddd.ddd.ddd.ddd\0" */

#define IP_PROTOCOL_ICMP 0x01
#define IP_PROTOCOL_TCP  0x06
#define IP_PROTOCOL_UDP  0x11
#define IP_PROTOCOL_RAW  0xff
#define IP_VERSION_IPV4     4

struct ip_hdr {
    uint8_t vhl_;
    uint8_t tos_;
    uint16_t len_;
    uint16_t id_;
    uint16_t offset_;
    uint8_t ttl_;
    uint8_t protocol_;
    uint16_t sum_;
    ipaddr_t src_;
    ipaddr_t dst_;
    uint8_t options_[0];
};
// END

// Build packet checksum (MIT's xv6 implementation).
uint16_t cksum16 (uint16_t *data, uint16_t size, uint32_t init);

// IP packet tx/rx request handlers.
ssize_t ip_tx (protocol_metadata* meta, uint8_t protocol, const uint8_t *buf, size_t len, const ipaddr_t *dst);
void ip_rx(uint8_t *dgram, size_t dlen, protocol_metadata* meta);

extern arp_entry arp_table[4];


#endif