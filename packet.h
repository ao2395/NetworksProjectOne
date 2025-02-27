// packet.h
#ifndef PACKET_H_INCLUDED
#define PACKET_H_INCLUDED

// Packet type enum
enum packet_type {
    DATA,
    ACK,
};

// Control flags 
#define NONE    0x0    // No flags
#define FIN     0x1    // Finish flag - indicates end of file or connection termination
#define SYN     0x2    // Synchronize flag - used for connection establishment
#define ACK     0x4    // Acknowledgment flag
#define RST     0x8    // Reset flag

// TCP Header structure
typedef struct {
    int seqno;          // Sequence number
    int ackno;          // Acknowledgment number
    int ctr_flags;      // Control flags (FIN, SYN, ACK, RST)
    int data_size;      // Size of data in the packet
} tcp_header;

// Maximum Segment Size and Header Sizes
#define MSS_SIZE        1500
#define UDP_HDR_SIZE    8
#define IP_HDR_SIZE     20
#define TCP_HDR_SIZE    sizeof(tcp_header)
#define DATA_SIZE       (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE)

// Packet structure
typedef struct {
    tcp_header  hdr;
    char    data[0];    // Flexible array member
} tcp_packet;

// Function prototypes
tcp_packet* make_packet(int seq);
int get_data_size(tcp_packet *pkt);

#endif // PACKET_H_INCLUDED
