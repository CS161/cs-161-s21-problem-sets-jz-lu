#ifndef CHICKADEE_K_ICMP_HH
#define CHICKADEE_K_ICMP_HH
#include "k-ip.hh"
#include "types.h"

// ICMP implementation based on the xv6 MIT pandax and lwIP implementations.
// REf: icmp.c @ https://savannah.nongnu.org/git/?group=lwip,
// https://github.com/pandax381/xv6-net/

#define ICMP_TYPE_ECHOREPLY 0
#define ICMP_TYPE_DEST_UNREACH 3
#define ICMP_TYPE_SOURCE_QUENCH 4
#define ICMP_TYPE_REDIRECT 5
#define ICMP_TYPE_ECHO 8
#define ICMP_TYPE_TIME_EXCEEDED 11
#define ICMP_TYPE_PARAM_PROBLEM 12
#define ICMP_TYPE_TIMESTAMP 13
#define ICMP_TYPE_TIMESTAMPREPLY 14
#define ICMP_TYPE_INFO_REQUEST 15
#define ICMP_TYPE_INFO_REPLY 16
#define ICMP_CODE_NET_UNREACH 0
#define ICMP_CODE_HOST_UNREACH 1
#define ICMP_CODE_PROTO_UNREACH 2
#define ICMP_CODE_PORT_UNREACH 3
#define ICMP_CODE_FRAGMENT_NEEDED 4
#define ICMP_CODE_SOURCE_ROUTE_FAILED 5
#define ICMP_CODE_REDIRECT_NET 0
#define ICMP_CODE_REDIRECT_HOST 1
#define ICMP_CODE_REDIRECT_TOS_NET 2
#define ICMP_CODE_REDIRECT_TOS_HOST 3
#define ICMP_CODE_EXCEEDED_TTL 0
#define ICMP_CODE_EXCEEDED_FRAGMENT 1
#define ICMP_COPY_LEN(x) ((((x)->vhl & 0x0f) << 2) + 8)
#define ICMP_BUFSIZ IP_PAYLOAD_SIZE_MAX

struct icmp_hdr {
    uint8_t type_;
    uint8_t code_;
    uint16_t sum_;
    uint32_t buf_;
    uint8_t data_[]; // C++ "flex array" trick to access icmp body with a header ptr
};

// ICMP Tx/Rx handlers
int icmp_tx(uint8_t type, uint8_t code, uint32_t values, uint8_t *data, size_t len, ipaddr_t* dst);
void icmp_rx (uint8_t *packet, size_t plen, ipaddr_t *src, ipaddr_t *dst);


#endif

