#include "k-icmp.hh"
#include "kernel.hh"
#include "k-netdriver.hh"
#include "k-ip.hh"


// icmp_tx(type, code, values, data, len, dst)
//    ICMP tx handler. type: ECHO or ECHO_REPLY, code: usually 0,
//    data: confirmation payload, dst: dest addr for protocol (for us this is just IP).
int icmp_tx (uint8_t type, uint8_t code, uint32_t values, uint8_t* data, size_t len, ipaddr_t* dst) {
    if (NET_PARANOIA > 4) {
         log_printf("[icmp-tx] about to init buffer inside of icmp tx\n");
    }
    // Allocate an ICMP buffer.
    struct icmp_hdr *hdr;
    size_t msg_len;
    uint8_t* buf = (uint8_t*) kalloc(sizeof(uint8_t)*ICMP_BUFSIZ);
    if (!buf) {
        if (NET_PARANOIA) {
            log_printf("[icmp-tx] could not allocated icmp buffer\n");
        }
        return 0;
    }

    // Cast the buf into a ICMP header and initialize it.
    hdr = (struct icmp_hdr*) buf;
    hdr->type_ = type; // ECHO or ECHO_REPLY
    hdr->code_ = code; // usually 0
    hdr->sum_ = 0; // initially cleared
    hdr->buf_ = values; // contains concatenated [id | seq]

    if (NET_PARANOIA > 2) {
        log_printf("[icmp-tx] hdr->data_: 0x%x, data: 0x%x\n", hdr->data_, data);
    }

    // Copy in the confirmation payload and update metadata.
    memcpy(hdr->data_, data, len);
    msg_len = sizeof(icmp_hdr) + len;
    hdr->sum_ = cksum16((uint16_t *)hdr, msg_len, 0);

    // Pass handling to IP transmitter and free buffer upon completion.
    ip_tx(nic->meta_, IP_PROTOCOL_ICMP, (uint8_t*) hdr, msg_len, dst);
    kfree(buf);
    return 0;
}


// icmp_rx(packet, plen, src, dst)
//    Handle incoming packets for ICMP messages (i.e. basically just ping msgs)
//    Respond to echo requests.
void icmp_rx (uint8_t *packet, size_t plen, ipaddr_t *src, ipaddr_t *dst) {
    if (NET_PARANOIA > 4) {
        log_printf("[icmp-rx] icmp rx entry\n");
    }
    (void) dst; // dst is just Chickadee's IP, so it is ignored, we use src

    // Validate packet length.
    if (plen < sizeof(icmp_hdr)) {
        if (NET_PARANOIA) {
            log_printf("[icmp-rx] error icmp recieved packet is smaller than header!\n");
        }
        return;
    }

    icmp_hdr *hdr = (icmp_hdr*) packet;
    switch (hdr->type_) {
        case ICMP_TYPE_ECHO: { // Respond to machines pinging Chickadee
            if (NET_PARANOIA > 2) {
                log_printf("[icmp-rx] icmp rx about to send tx\n");
                log_printf("[icmp-rx] hdr code %x, hdr buf: %x, hdr data: %x, plen: %x, src: %x\n ", 
                    hdr->code_, hdr->buf_, hdr->data_, plen - sizeof(icmp_hdr), src);
            }
            
            icmp_tx(ICMP_TYPE_ECHOREPLY, hdr->code_, hdr->buf_, hdr->data_, plen - sizeof(icmp_hdr), src);
            break;
        }

        case ICMP_TYPE_ECHOREPLY: { 
            uint8_t* ptr = (uint8_t*) src;
            char ip_str[16] = {0}; // buffer to store IP
            uint16_t seq = (uint16_t) hdr->buf_;
            uint16_t id = (uint16_t) (hdr->buf_ >> 16);
            snprintf(ip_str, 15, "%d.%d.%d.%d", ptr[0], ptr[1], ptr[2], ptr[3]); // IP to buffer

            // Print ping statistics to the console.
            console_printf("%lu bytes from %s: id=%i icmp_seq=%i ttl=%i type=ICMP_TYPE_ECHOREPLY\n", 
                plen, ip_str, id, seq, 255);
            break;
        }

        default: {
            if (NET_PARANOIA > 2) {
                log_printf("[icmp-rx] type not here: 0x%x\n", hdr->type_);
            }
            break;
        }
        return;
    }
}
