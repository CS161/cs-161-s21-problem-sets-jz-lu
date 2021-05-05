#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-pci.hh"
#include "k-e1000.hh"
#include "k-arp.hh"
#include "k-ip.hh"


// e1000state::byteswap16(vc)
//    Networking devices/ring buffers use big-endian, 
//    but Chickadee is little-endan; byteswap() converts.
uint16_t e1000state::byteswap16(uint16_t vc) {
    return (vc & 0x00ff) << 8 | (vc & 0xff00 ) >> 8;
}


// e1000state::eth_addr_extractor()
//    Dump e1000 MAC address in a formatted way into the buffer.
char* e1000state::eth_addr_extractor(char* buf) {
    if (!buf) {
        return nullptr;
    }
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", 
        addr_[0], addr_[1], addr_[2], addr_[3], addr_[4], addr_[5]);
    return buf;
}


// e1000state::find(addr)
//    Find E1000 NIC by iterating through PCI-e,
//    similar to ahcistate::find() but without ports.
e1000state* e1000state::find(int addr) {
    auto& pci = pcistate::get();
    for (; addr >= 0; addr = pci.next(addr)) {
        // 0x0200 is the signature for an e1000 
        // PCI-e subclass, so the search through the PCI-e occurs
        // until the NIC signature is found.
        // (Ref: https://pci-ids.ucw.cz/read/PD/02)
        if (pci.readw(addr + pci.config_subclass) != 0x0200) {
            continue;
        }
        return knew<e1000state>(addr);
    }
    return nullptr;
}


// e1000state:e1000state(addr)
//    Init e1000 using the corresponding PCI-e address.
//    Also init tx and rx ring bufs.
e1000state::e1000state(int pci_addr) {
	auto& pci = pcistate::get();
	pci_addr_ = pci_addr;
    pci.enable(pci_addr_); // tell PCI to enable the given addr

    // QEMU default MMIO region is bar0; configure device via PCI-e config space method.
    // (Ref: https://pdos.csail.mit.edu/6.828/2017/labs/lab6/)
    auto pa = pci.readl(pci_addr_ + pci.config_bar0);
    volatile uint32_t dr = pa2ka(pa);
    memio_ = dr;

    // Get MAC address.
    get_eeprom_addr();

    int intr_pin = ((pci.readl(pci_addr_ + 0x3C) >> 8) & 0xFF) - 1;
    irq_ = machine_pci_irq(pci_addr_, intr_pin);
    // here we make sure to figure out what the interrupt pin is going to be
    // because that will be used by my exception handler to figure out when
    // an exeception is being triggered by a network packet
    // we also register the irq_ with the APIC here too.

    // Setup at least 128 receive descriptors in rx ring (See Section B.10 of Ref above).
    for (int n = 0; n < 128; n++) {
        // E1000_MTA: multi-cast table array
        reg_write(E1000_MTA + (n << 2), 0);
    }

    rx_init();
    tx_init();

    // Allocate and initialize the const struct protocol_metadata.
    meta_ = knew<protocol_metadata>();

    meta_->type_ = NETDEV_TYPE_ETHERNET;
    meta_->mtu_ = MAX_PAYLOAD_SIZE;
    meta_->flags_ = NETDEV_FLAG_BROADCAST | NETDEV_FLAG_RUNNING;
    meta_->hlen_ = HDR_SZ;
    meta_->alen_ = ADDR_LEN;

    // Store MAC address as well.
    memcpy(meta_->addr_, addr_, 6);
}


// e1000state:microdelay(amount)
//    Pause for approximately t microseconds
//    Used for hardware delays in ring buf IO.
void e1000state::microdelay(int t) {
    uint64_t x = rdtsc() + (uint64_t) t * 10000;
    while ((int64_t) (x - rdtsc()) > 0) {
        asm volatile("pause");
    }
}


// e1000state:reg_read(reg)
//    Read a MMIO register spcified by a physical address.
unsigned int e1000state::reg_read(uint16_t reg) {
	return *(volatile uint32_t *)pa2ka(memio_ + reg);
}


// e1000state:reg_write
//    Write `val` to MMIO reg (a physical address).
void e1000state::reg_write(uint16_t reg, uint32_t val) {
	*(volatile uint32_t *)pa2ka(memio_ + reg) = val;
}


// e1000state:eeprom_read()
//    Access the NIC's 'electronically erasable programmable read-only memory'
//    Used to read in MAC address in 16 bits at a time.
uint16_t e1000state::eeprom_read(uint8_t addr) {
    uint32_t rd;
    // Cat 2 16-bit uints into one 32-bit uint and configure which 16 bits to read.
    reg_write(E1000_EERD, E1000_EERD_READ | addr << E1000_EERD_ADDR);

    // Wait for hardware response.
    while (!((rd = reg_read(E1000_EERD)) & E1000_EERD_DONE)) {
        microdelay(1);
    }
    return (uint16_t)(rd >> E1000_EERD_DATA);
}


// e1000state::get_eeprom_addr()
//    Read MAC address into e1000state::addr_.
void e1000state::get_eeprom_addr() {
    uint16_t double_char;
    //* Note: 3 * 2 = 6 == ADDR_LEN
    //  Read 16 bits at a time in accordance to eeprom_read()
    //  then split them into uint8_t == char.
    for (int n = 0; n < 3; n++) {
        double_char = eeprom_read(n);
        addr_[n*2+0] = (double_char & 0xff);
        addr_[n*2+1] = (double_char >> 8) & 0xff;
    }
}


// e1000state::rx_init()
//     Configure the DMA region and set up rx ring buffer.
void e1000state::rx_init() {
    // Ref: Section A.4 of https://pdos.csail.mit.edu/6.828/2017/labs/lab6/
    // and https://www.intel.com/content/www/us/en/support/articles/000005480/ethernet-products.html
    for(int n = 0; n < RX_RING_SIZE; n++) {
        // Clear the region first.
        memset(&rxring_[n], 0, sizeof(rx_desc));
        rxring_[n].addr = kptr2pa(kalloc(sizeof(uint64_t)));
    }

    // Configure rx descriptors starting at the base of the ring buf.
    uint64_t base = kptr2pa(rxring_);
    // Lower RX desc base address.
    reg_write(E1000_RDBAL, (uint32_t)(base & 0xffffffff));
    // Higher RX desc.
    reg_write(E1000_RDBAH, (uint32_t)(base >> 32));
    // Set length of ring.
    reg_write(E1000_RDLEN, (uint32_t)(RX_RING_SIZE * sizeof(rx_desc))); 
    // Set desc head.
    reg_write(E1000_RDH, 0);
    // Set the tail to be right behind (+15 entries after) the head.
    reg_write(E1000_RDT, RX_RING_SIZE-1);

    // Enable bad packet storage, unicast+multicast promiscuous mode,
    // the rx min threshold size. Strip the eth crc32, enable long packet recieving,
    // enable broadcast. Set the rx buffer size to 2048 = (16 * sizeof(desc))
    reg_write(E1000_RCTL, (E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE |
        E1000_RCTL_RDMTS_HALF | E1000_RCTL_SECRC | E1000_RCTL_LPE | E1000_RCTL_BAM |
        E1000_RCTL_SZ_2048 | 0));
}


// e1000state::tx_init()
//   Configure TX ring buf.
void e1000state::tx_init() {
    // See rx_init() for 
    for (int n = 0; n < TX_RING_SIZE; n++) {
        memset(&txring_[n], 0, sizeof(tx_desc));
    }

    // See comments of rx_init(), the below are analogous
    uint64_t base = kptr2pa(txring_); 
    reg_write(E1000_TDBAL, (uint32_t)(base & 0xffffffff));
    reg_write(E1000_TDBAH, (uint32_t)(base >> 32) );
    reg_write(E1000_TDLEN, (uint32_t)(TX_RING_SIZE * sizeof(struct tx_desc)));
    reg_write(E1000_TDH, 0);
    // Unlike rx ring, the tail starts at the same place as the head.
    reg_write(E1000_TDT, 0);

    // See part B of https://pdos.csail.mit.edu/6.828/2017/labs/lab6/.
    // Pad short packets with a payload min of 46, total being 64 (including the hdr),
    reg_write(E1000_TCTL, (E1000_TCTL_PSP | 0));
}


// e1000state::bootup()
//    Activate ring buffers and enable interrupts.
int e1000state::bootup() {
    // First write to the interrupt mask setter the value
    // of a timer interrupt (will be raised for incoming packets)
    reg_write(E1000_IMS, E1000_IMS_RXT0);
    // Read so device clears existing interrupts.
    reg_read(E1000_ICR);

    // Enable by writing the enable value to the RX and TX control regs.
    reg_write(E1000_RCTL, reg_read(E1000_RCTL) | E1000_RCTL_EN);
    reg_write(E1000_TCTL, reg_read(E1000_TCTL) | E1000_TCTL_EN);

    // Complete boot by writing to main control.
    reg_write(E1000_CTL, reg_read(E1000_CTL) | E1000_CTL_SLU);
    return 0;
}


// e1000state::tx_package(data, len)
//    Prepares and formats packets; sends off eth frames.
ssize_t e1000state::tx_package(uint8_t *data, size_t len) {
    // Grab location of tail.
    uint32_t tail = reg_read(E1000_TDT);

    if (NET_PARANOIA > 4) {
        log_printf("[e1000state-tx_package] tail (0): %lu\n", tail);
    }

    tx_desc *desc = &txring_[tail];
    if (NET_PARANOIA > 4) {
        log_printf("[e1000state-tx_package] current device status: 0x%x\n", 
            reg_read(E1000_TXD_CMD_RS));
    }

    // Prepare the buffer entry and indicate a send.
    desc->addr = kptr2pa(data); 
    desc->length = len;
    desc->status = 0;
    desc->cmd = (E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS | E1000_TXD_CMD_IFCS);

    if (NET_PARANOIA > 3) {
        log_printf("[e1000state-tx_package] %u bytes data transmit\n", desc->length);
    }

    // After we write to a packet, move that tail.
    reg_write(E1000_TDT, (tail + 1) % TX_RING_SIZE);
    tail = reg_read(E1000_TDT);

    if (NET_PARANOIA > 4) {
        log_printf("[e1000state-tx_package] tail (1): %lu\n", tail);
    }

    // Wait for the entry to be transmitted.
    while(!(desc->status & 0x0f)) {
        if (NET_PARANOIA > 4) {
            log_printf("[e1000state-tx_package] status: %lu, masked: %lu\n", 
                desc->status, desc->status & 0x0f);
        }
        microdelay(1);
    }

    return len;
}


// e1000state::rx_package()
//    Handles all of the packets that trigger interrupts upon arrival.
//    Ref: Based off instructions from https://github.com/mit-pdos/xv6-public
void e1000state::rx_package() {
    if (NET_PARANOIA > 4) {
        log_printf("[e1000state-rx_package] rx package handler called\n");
    }

    int short_package_sz = 60;
    while (true) {
        uint32_t tail = (reg_read(E1000_RDT)+1) % RX_RING_SIZE; // tx tail index
        rx_desc* desc = &rxring_[tail];

        // Break if status is done (DD).
        if (!(desc->status & E1000_RXD_STAT_DD)) {
            break;
        } 
        do {
            if (desc->errors) {
                break;
            }
            if (!(desc->status & E1000_RXD_STAT_EOP)) {
                // unsupported packet types
                break;
            }
            if (desc->length < short_package_sz) {
                // package size is too small
                break;
            }
            rx_ethernet_formatter(meta_, (uint8_t*)desc->addr, desc->length);
        } while (0);

        // Clear descriptor status and update tail.
        desc->status = (uint16_t)(0);
        reg_write(E1000_RDT, tail);
    }
}


// e1000state::intr()
//    Interrupt handler, called by the kernel exception handler.
void e1000state::intr(void) {
    int icr_val;
    icr_val = reg_read(E1000_ICR);
    if (icr_val & E1000_ICR_RXT0) {
        rx_package(); // actually handle packet

        // Clear pending interrupts.
        reg_read(E1000_ICR);
    }
    lapicstate::get().ack(); // ack intr
}


// e1000state::tx_ethernet_formatter(meta, type, payload, plen, *dst)
//    Args: meta: default device info, type: packet type,
//    payload: frame ptr, plen: packet length, and dst: dest ptr.
//    Sets up the src, dst and code for the ethernet packet.
ssize_t e1000state::tx_ethernet_formatter(protocol_metadata* meta, uint16_t type, 
    const uint8_t* payload, size_t plen, const void *dst) {

    uint8_t __attribute__((aligned(16))) frame[MAX_FRAME_SIZE];

    eth_header* header;
    size_t flen; // file length

    if (NET_PARANOIA > 1) {
        log_printf("[e1000state-tx-eth] function formatter called\n");
    }

    if (!(payload && dst) || plen > MAX_PAYLOAD_SIZE) {
        return -1;
    }

    // Clear the frame, then access offsets in the frame using the "flex array" trick.
    memset(frame, 0, sizeof(frame));
    header = (eth_header *)frame;
    // Put destination and source MAC addresses into header.
    memcpy(header->dst_addr_, dst, ADDR_LEN);
    memcpy(header->src_addr_, meta_->addr_, ADDR_LEN);
    header->type_ = byteswap16(type); // byte swap from Chickadee to networking
    // Add size header offset to header ptr, so that the payload gets copied in correctly.
    memcpy(header + 1, payload, plen);

    // Ensure payload size is at least the minimum.
    if (plen < MIN_PAYLOAD_SIZE) {
        plen = MIN_PAYLOAD_SIZE;
    }

    flen = sizeof(eth_header) + plen;
    return tx_package(frame, flen);
}


// e1000state::rx_ethernet_formatter(meta, frame, flen)
//    Read device data/metadata and forward to type handler.
//    Currently only handles ARP and ICMP.
void e1000state::rx_ethernet_formatter(protocol_metadata* meta, uint8_t *frame, size_t flen) {
    struct eth_header* header;
    uint8_t* payload;
    size_t plen;

    if (flen < sizeof(eth_header)) {
        if (NET_PARANOIA) {
            log_printf("[e1000state-rx-eth] frame is smaller than a header?\n");
        }
        return;
    }

    header = pa2kptr<eth_header*>((uintptr_t) frame); // frame is physical address ptr, make it kptr
    if (memcmp(addr_, header->dst_addr_, ADDR_LEN) != 0) {
        if (NET_PARANOIA) {
            log_printf("[e1000state-rx-eth] destination is different than intended?\n");
        }
    }

    // Set plen to the correct size.
    payload = (uint8_t *)(header + 1);
    plen = flen - sizeof(eth_header);

    // Forward the packet to handler.
    switch (byteswap16(header->type_)) {
        case ETHERNET_TYPE_ARP: {
            return arp_functions::arp_rx(payload, plen, meta_);
        }
        case ETHERNET_TYPE_IP: {
            return ip_rx(payload, plen, meta_);
        }
    }
}
