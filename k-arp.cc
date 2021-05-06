#include "k-arp.hh"

static uint32_t chickadee_addr[4] = {172, 17, 0, 15}; // Chickadee IP
static uint32_t default_addr[4] = {172, 17, 0, 1}; // Docker exit point "docker0"
static uint32_t tap_addr[4] = {172, 17, 0, 3}; // Tap

// arp_func::inet_pton(ip_vals)
//    Converts array-formatted ip address with uint32_t values
//    into a single uint32_t. Used to format send/recieve addrs of ARP packets.
ipaddr_t arp_func::inet_pton(uint32_t ip_vals[4]) {
    ipaddr_t ipv4 = (ip_vals[3] << 24) + (ip_vals[2] << 16) + (ip_vals[1] << 8) + ip_vals[0];
    return ipv4;
}


// arp_func::arp_send_request(tpa)
//    Send ARP packet to `tpa`. Formats AP packet and assigns dest MAC address.
//    Send addr (spa) is Chickadee's IP. SHA (source hardware address) is Chickadee MAC.
int arp_func::arp_send_request(ipaddr_t tpa) {
    if (!tpa) {
        return -1;
    }
    arp_eth request; // header for ARP
    request.hdr.hln = ADDR_LEN; // length of hardware (MAC) addr
    request.hdr.pln = IP_ADDR_LEN;

    // Byte swap as these are expected to be big-endian.
    request.hdr.hrd = e1000state::byteswap16(ARP_HRD_ETHERNET);
    request.hdr.pro = e1000state::byteswap16(ETHERNET_TYPE_IP);
    request.hdr.op = e1000state::byteswap16(ARP_OP_REQUEST);

    // Give packet sender and destination hardware + IP addresses. 
    memcpy(request.sha, nic->addr_, ADDR_LEN);
    request.spa = inet_pton(chickadee_addr);
    memset(request.tha, 0, ADDR_LEN);
    request.tpa = tpa;
    uint64_t broadcast_addr = -1; // 0xFFFFFFFF

    // Format the packet with destination a broadcast.
    nic->tx_ethernet_formatter(nic->meta_, ETHERNET_TYPE_ARP, 
    	(uint8_t *)&request, sizeof(request), &broadcast_addr);
    return 0;
}


// arp_func::arp_send_reply(tha, tpa, dst)
//      Send reply packet with `tha`/`tpa` the MAC/IP address of the receiver (dest).
int arp_func::arp_send_reply(const uint8_t *tha, const ipaddr_t *tpa, const uint8_t *dst) {
    if (!tha || !tpa) {
        // Note that dst can be null.
        return -1;
    }
    arp_eth reply;

    // See comments in arp_send_request().
    reply.hdr.hln = ADDR_LEN;
    reply.hdr.pln = IP_ADDR_LEN;
    reply.hdr.hrd = e1000state::byteswap16(ARP_HRD_ETHERNET);
    reply.hdr.pro = e1000state::byteswap16(ETHERNET_TYPE_IP);
    reply.hdr.op = e1000state::byteswap16(ARP_OP_REPLY);

    memcpy(reply.sha, nic->meta_->addr_, ADDR_LEN);
    reply.spa = inet_pton(chickadee_addr);
    memcpy(reply.tha, tha, ADDR_LEN);
    reply.tpa = *tpa;

    // Note: addr is not a broadcast anymore.
    nic->tx_ethernet_formatter(nic->meta_, ETHERNET_TYPE_ARP, (uint8_t *)&reply, sizeof(reply), dst);
    return 0;
}


// arp_func::arp_rx(packet, plen, meta)
//    Validates pointers, and either sends back a response or
//    updates the ARP table.
void arp_func::arp_rx(uint8_t *packet, size_t plen, protocol_metadata* meta) {
    // Packet length should not be smaller than the struct sz.
    if (plen < sizeof(arp_eth)) {
        return;
    }
    arp_eth* message = (arp_eth*) packet;

    // Ensure other lengths, type, protocols are valid.
    if (message->hdr.hln != ADDR_LEN || message->hdr.pln != IP_ADDR_LEN) {
        return;
    }
    if (e1000state::byteswap16(message->hdr.hrd) != ARP_HRD_ETHERNET || 
        e1000state::byteswap16(message->hdr.pro) != ETHERNET_TYPE_IP) {
        return;
    }
    
    // Reply to ARP requests; otherwise update the ARP table.
    if (e1000state::byteswap16(message->hdr.op) == ARP_OP_REQUEST) {
        arp_send_reply(message->sha, &message->spa, message->sha);
    } else {
        if (inet_pton(default_addr) == message->spa) { // update default
            arp_table[0].pa = message->spa;
            memcpy(arp_table[0].ha, &message->sha, ADDR_LEN);
        } else if (inet_pton(tap_addr) == message->spa) { // update tap
            arp_table[1].pa = message->spa;
            memcpy(arp_table[1].ha, &message->sha, ADDR_LEN);
        }
    }
}

