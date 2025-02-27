#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"


tcp_packet *recvpkt;
tcp_packet *sndpkt;
int expectedseq = 0;
int highest_seqno_received = 0;

#define WINDOW_SIZE 10

// Buffer for out-of-order packets
typedef struct {
    tcp_packet packet;
    int valid;  // Flag to indicate if this buffer slot contains a valid packet
    int seqno;  // Store the sequence number for easier sorting and access
} packet_buffer;

// Create a buffer to store out-of-order packets
packet_buffer recv_buffer[WINDOW_SIZE];

// Initialize the buffer
void init_recv_buffer() {
    for (int i = 0; i < WINDOW_SIZE; i++) {
        recv_buffer[i].valid = 0;
        recv_buffer[i].seqno = -1;
    }
}

// Free resources in the buffer
void cleanup_recv_buffer() {
    // Free any allocated resources in the buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        recv_buffer[i].valid = 0;
        recv_buffer[i].seqno = -1;
    }
}

// Find a packet in the buffer by sequence number
int find_packet_in_buffer(int seqno) {
    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (recv_buffer[i].valid && recv_buffer[i].seqno == seqno) {
            return i;  // Return the index of the found packet
        }
    }
    return -1;  // Packet not found
}

// Find the next expected packet in buffer
int find_next_expected_packet() {
    return find_packet_in_buffer(expectedseq);
}

// Store a packet in the buffer
int store_packet_in_buffer(tcp_packet *pkt) {
    // Find an empty slot
    int empty_slot = -1;
    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (!recv_buffer[i].valid) {
            empty_slot = i;
            break;
        }
    }
    
    if (empty_slot == -1) {
        // Buffer is full, need to implement a replacement strategy
        // For now, let's replace the packet with the highest sequence number
        int highest_idx = 0;
        int highest_seq = recv_buffer[0].seqno;
        
        for (int i = 1; i < WINDOW_SIZE; i++) {
            if (recv_buffer[i].seqno > highest_seq) {
                highest_seq = recv_buffer[i].seqno;
                highest_idx = i;
            }
        }
        
        // Only replace if the new packet has a lower sequence number
        if (pkt->hdr.seqno < highest_seq) {
            empty_slot = highest_idx;
        } else {
            return -1;  // Cannot store this packet
        }
    }
    
    // Copy the packet to the buffer
    memcpy(&recv_buffer[empty_slot].packet, pkt, sizeof(tcp_packet));
    recv_buffer[empty_slot].valid = 1;
    recv_buffer[empty_slot].seqno = pkt->hdr.seqno;
    
    // Track highest sequence number
    if (pkt->hdr.seqno > highest_seqno_received) {
        highest_seqno_received = pkt->hdr.seqno;
    }
    
    return empty_slot;
}

// Process in-order packets from the buffer
void process_buffer(FILE *fp) {
    // Keep processing packets as long as we find expected ones
    while (1) {
        int idx = find_next_expected_packet();
        if (idx == -1) {
            break;  // No more expected packets found
        }
        
        // Process the packet
        tcp_packet *pkt = &recv_buffer[idx].packet;
        
        // Log processing
        printf("Processing buffered packet with seqno: %d, size: %d\n", 
               pkt->hdr.seqno, pkt->hdr.data_size);
        
        // Write the data to the file
        fseek(fp, pkt->hdr.seqno, SEEK_SET);
        fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
        
        // Update the expected sequence number
        expectedseq += pkt->hdr.data_size;
        
        // Mark the buffer slot as empty
        recv_buffer[idx].valid = 0;
        recv_buffer[idx].seqno = -1;
    }
}

// Check if a packet is within our receiving window
int is_within_window(int seqno) {
    return (seqno >= expectedseq && 
            seqno < expectedseq + (WINDOW_SIZE * DATA_SIZE));
}

// Create an ACK packet with cumulative acknowledgment
tcp_packet* create_ack_packet(int cumulative_ack) {
    tcp_packet *pkt = make_packet(0);
    pkt->hdr.ackno = cumulative_ack;
    pkt->hdr.ctr_flags = ACK;
    return pkt;
}

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval, sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");
    init_recv_buffer(); // Initialize the receiver buffer
    clientlen = sizeof(clientaddr);
    
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        
        // Check for EOF packet
        if (recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            
            // Process any remaining buffered packets
            process_buffer(fp);
            
            // Send final ACK
            sndpkt = create_ack_packet(expectedseq);
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            printf("Final ACK sent: %d\n", sndpkt->hdr.ackno);
            free(sndpkt);
            
            fclose(fp);
            break;
        }
        
        // Log the packet info
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        printf("Received packet - seqno: %d, datasize: %d\n", recvpkt->hdr.seqno, recvpkt->hdr.data_size);
        
        if (recvpkt->hdr.seqno == expectedseq) {
            // In-order packet - process immediately
            printf("In-order packet received (seqno: %d)\n", recvpkt->hdr.seqno);
            
            // Write data to file
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            
            // Update expected sequence number
            expectedseq += recvpkt->hdr.data_size;
            
            // Process any buffered packets that may now be in order
            process_buffer(fp);
            
            // Send cumulative ACK
            sndpkt = create_ack_packet(expectedseq);
            
            printf("Sending cumulative ACK: %d\n", sndpkt->hdr.ackno);
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            free(sndpkt);
        } else {
            // Out-of-order packet
            printf("Out-of-order packet received (seqno: %d, expected: %d)\n", 
                  recvpkt->hdr.seqno, expectedseq);
            
            if (is_within_window(recvpkt->hdr.seqno)) {
                // Buffer the packet for later processing
                if (store_packet_in_buffer(recvpkt) >= 0) {
                    printf("Packet with seqno %d stored in buffer\n", recvpkt->hdr.seqno);
                } else {
                    printf("Failed to store packet with seqno %d (buffer full)\n", recvpkt->hdr.seqno);
                }
            } else {
                printf("Packet with seqno %d outside window, discarding\n", recvpkt->hdr.seqno);
            }
            
            // Send duplicate ACK for the expected sequence number
            sndpkt = create_ack_packet(expectedseq);
            
            printf("Sending duplicate ACK: %d\n", sndpkt->hdr.ackno);
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            free(sndpkt);
        }
    }
    
    // Clean up resources
    cleanup_recv_buffer();
    
    return 0;
}