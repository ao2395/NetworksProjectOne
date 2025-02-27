enum packet_type { //making an enumeration that has 3 potential values: DATA , ACK or, FIN
    DATA, //assigned O
    ACK, //assigned 1
    FIN, //assigned 2
};

typedef struct { //defining a struct in C that has the header information for the TCP packets
    int seqno; // sequence number to fint the position of the 1st data byte in packet
    int ackno; //ACK number for the next sequence number the receiver is expectign to recieve
    int ctr_flags; //stores the tpye of the packet
    int data_size; //stores the size of the packet in bytes
}tcp_header;

#define MSS_SIZE    1500 //we use MSS in the C files, here we define its size to be 1500
#define UDP_HDR_SIZE    8 //set the UDP header size to 8
#define IP_HDR_SIZE    20 //sets the iP header as 20 bytes, this is the min size for IPv4 headers without options
#define TCP_HDR_SIZE    sizeof(tcp_header) //definign this to the size of the tcp_deader struct in bytes
#define DATA_SIZE   (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE) //this calculates the max size available for data in a packet, done by subtracting all the header sizes from MSS
typedef struct { //defining a struct called tcp_packet to represent a complete packet with:
    tcp_header  hdr; // a tcp_header struct that has the packet header details
    char    data[0]; //making a flexible array member
}tcp_packet;

tcp_packet* make_packet(int seq);//function to make a new packet the seq numbe given
int get_data_size(tcp_packet *pkt); //function to get size of data in packet
