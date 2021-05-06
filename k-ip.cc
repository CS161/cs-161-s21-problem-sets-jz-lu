#include "k-ip.hh"
#include "k-netdriver.hh"
#include "k-icmp.hh"
#include <atomic>

static uint32_t chickadee_addr[4] = {172, 17, 0, 15};
static uint32_t default_addr[4] = {172, 17, 0, 1};

#define IP_ADDR_BROADCAST 0xffffffff
#define IP_ADDR_ANY 0x00000000

// Analogous to method of assigning tgid's in syscall_fork() @ kernel.cc.
std::atomic<uint16_t> ip_id(128);
static uint16_t ip_generate_id (void) {
    return ++ip_id; 
}

// NOTE: some of this is based off of the references 
// given in the associated .hh file.

// cksum16 (data, size, init)
//    Ported from common.c @ https://github.com/mit-pdos/xv6-public.
uint16_t cksum16 (uint16_t *data, uint16_t size, uint32_t it) {
    uint32_t sum;
    sum = it;
    while(size > 1) {
        sum += *(data++);
        size -= 2;
    }
    if(size) {
        sum += *(uint8_t *)data;
    }
    sum  = (sum & 0xffff) + (sum >> 16);
    sum  = (sum & 0xffff) + (sum >> 16);
    return ~(uint16_t)sum;
}


// ip_tx_eth(meta, packet, plen, dst)
//    Sends packet to either the broadcast addr or
//    the default gateway MAC addr.
static int ip_tx_eth(protocol_metadata* meta, uint8_t *packet, 
    size_t plen, const ipaddr_t *dst) {
    uint8_t opt[128] = {};

    ip_hdr* hdr = (ip_hdr*) packet;

    if (hdr->dst_ <= arp_func::inet_pton(default_addr) &&
        hdr->dst_ >= arp_func::inet_pton(chickadee_addr)) {
        log_printf("on link\n");
        uint64_t broadcast = -1;
        memcpy(opt, &broadcast, meta->alen_);
    } else {
        if (arp_table[0].pa != IP_ADDR_BROADCAST) {
            log_printf("off link\n");
            memcpy(opt, arp_table[0].ha, meta->alen_);
        } else {
            log_printf("on link but should be off link\n");
            uint64_t broadcast = -1;
            memcpy(opt, &broadcast, meta->alen_);
        }
    }
    // here we set the MAC address to the opt buffer and send that in as the dst MAC addr
    nic->tx_ethernet_formatter(meta, ETHERNET_TYPE_IP, packet, plen, (void *)opt);
    return 1;
}


// ip_tx_helper(meta, protocol, buf, len, src, dst, nexthop, id, offset)
//    Sets flags for the IP packet and then passes it on
//    to the eth IP handler for send.
static int ip_tx_helper(protocol_metadata* meta, uint8_t protocol, const uint8_t *buf, 
	size_t len, const ipaddr_t *src, const ipaddr_t *dst, const ipaddr_t *nexthop, 
	uint16_t id, uint16_t offset) {

    // Allocate and initialize a new packet.
    uint8_t* packet = (uint8_t*) kalloc(sizeof(uint8_t)*4096);
    ip_hdr* hdr = (ip_hdr*) packet;
    uint16_t hlen = sizeof(ip_hdr);
    hdr->vhl_ = (IP_VERSION_IPV4 << 4) | (hlen >> 2);
    hdr->tos_ = 0; // type of service

    hdr->len_ = e1000state::byteswap16(hlen + len);
    hdr->id_ = e1000state::byteswap16(id);
    hdr->offset_ = e1000state::byteswap16(offset);
    hdr->ttl_ = 0xff;
    hdr->protocol_ = protocol;
    hdr->sum_ = 0;
    hdr->src_ = *src;
    hdr->dst_ = *dst;

    hdr->sum_ = cksum16((uint16_t *)hdr, hlen, 0);
    memcpy(hdr + 1, buf, len);

    // Copy buffer payload into the packet, send it off, and 
    // free the packet from memorywhen done.
    int ret = ip_tx_eth(nic->meta_, (uint8_t*) packet, hlen + len, nexthop);
    kfree(packet);
    return ret;
}


// ip_tx(meta, protocol, buf, len, dst)
//    Handles tx requests based on the protocol. Sends payload
//    to the destination address.
ssize_t ip_tx (protocol_metadata* meta, uint8_t protocol, 
    const uint8_t *buf, size_t len, const ipaddr_t *dst) {

    ipaddr_t *nexthop = NULL, *src = NULL;
    uint16_t id, flag, offset;
    size_t done, slen;
    ipaddr_t src_data;

    if (NET_PARANOIA > 4) {
        log_printf("[ip-tx] ip tx called\n");
    }

	src_data = arp_func::inet_pton(chickadee_addr);
    src = &src_data;
    nexthop = nullptr;
    id = ip_generate_id();

    for (done = 0; done < len; done += slen) {
        slen = min((len - done), (size_t)(nic->meta_->mtu_ - IP_HDR_SIZE_MIN));
        flag = ((done + slen) < len) ? 0x2000 : 0x0000;
        offset = flag | ((done >> 3) & 0x1fff);
        int ret = ip_tx_helper(nic->meta_, protocol, buf + done, slen, src, dst, nexthop, id, offset);
        if (ret < 0) {
            return ret;
        }
    }
    return len;
}


// ip_rx(packet, dlen, meta)
//    Handles most incoming IP packets and forwards to handler.
void ip_rx(uint8_t *packet, size_t dlen, protocol_metadata* meta) {
    if (dlen < sizeof(ip_hdr)) {
        // then we have a logical check to make sure the header is not bigger than the packet
        return;
    }
    
    // Initialize a new header.
    struct ip_hdr *hdr;
    uint16_t hlen, offset;
    uint8_t *payload;
    size_t plen;
    hdr = (ip_hdr*) packet;

    // Validate packet time to live (ttl_), version, lengths, and checksum.
    if (!hdr->ttl_) {
        if (NET_PARANOIA) {
            log_printf("[ip_rx] the time to live expired\n");
        }
        return;
    }
    if ((hdr->vhl_ >> 4) != IP_VERSION_IPV4) {
        if (NET_PARANOIA) {
            log_printf("[ip_rx] only ipv4 packets are supported\n");
        }
        return;
    }
    hlen = (hdr->vhl_ & 0x0f) << 2;
    if (dlen < hlen || dlen < e1000state::byteswap16(hdr->len_)) {
        if (NET_PARANOIA) {
            log_printf("[ip_rx] ip packet field lengths are incorrect\n");
        }
        return;
    }
    if (cksum16((uint16_t *)hdr, hlen, 0) != 0) {
        if (NET_PARANOIA) {
            log_printf("[ip_rx] the packet's checksum is invalid\n");
        }
        return;
    }

    payload = (uint8_t*) hdr + hlen;
    plen = e1000state::byteswap16(hdr->len_) - hlen;
    offset = e1000state::byteswap16(hdr->offset_);

    // Ensure there is no fragmentation-based discrepancy.
    if (offset & 0x2000 || offset & 0x1fff) {
        if (NET_PARANOIA) {
            log_printf("[ip_rx] the packet is fragmented (no support)\n");
        }
        return;
    }

    if (NET_PARANOIA > 3) {
        log_printf("[ip_rx] all checks have passed\n");
        log_printf("[ip_rx] ip protocol type: %x\n", hdr->protocol_);
    }

    switch (hdr->protocol_) {
    	case IP_PROTOCOL_ICMP: {
    		icmp_rx(payload, plen, &hdr->src_, &hdr->dst_);
    	}
        // ... that's the only protocol we handle at the moment
    }
}

