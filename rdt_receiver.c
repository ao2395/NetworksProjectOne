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

#define BUFFER_SIZE 20 // Size of the packet buffer

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;
int expectedseq = 0;
int last_ack_sent = 0;  // Track the last ACK we sent

// Arrays for packet buffering
tcp_packet* packet_buffer[BUFFER_SIZE]; // Array to store out-of-order packets
int buffer_seqno[BUFFER_SIZE];          // Sequence numbers of buffered packets
int buffer_used[BUFFER_SIZE];           // Whether the buffer slot is in use

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
            (const void *)&optval , sizeof(int));

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

    clientlen = sizeof(clientaddr);
    int eof_received = 0;
    
    // Initialize the buffer arrays
    for (int i = 0; i < BUFFER_SIZE; i++) {
        packet_buffer[i] = NULL;
        buffer_seqno[i] = -1;
        buffer_used[i] = 0;
    }
    
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
        
        // Handle EOF packet
        if (recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File packet received");
            printf("EOF packet received, sending ACK\n");
            
            // Before ending, process any packets in the buffer that we can
            int processed;
            do {
                processed = 0;
                for (int i = 0; i < BUFFER_SIZE; i++) {
                    if (buffer_used[i] && buffer_seqno[i] == expectedseq) {
                        // Found a packet that can now be processed
                        tcp_packet *pkt = packet_buffer[i];
                        
                        // Write data to file
                        fseek(fp, pkt->hdr.seqno, SEEK_SET);
                        fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
                        fflush(fp);
                        
                        printf("Processed buffered packet with seqno %d\n", pkt->hdr.seqno);
                        
                        // Update expected sequence number
                        expectedseq += pkt->hdr.data_size;
                        
                        // Free buffer slot
                        free(packet_buffer[i]);
                        packet_buffer[i] = NULL;
                        buffer_seqno[i] = -1;
                        buffer_used[i] = 0;
                        
                        processed = 1;
                        break;
                    }
                }
            } while (processed); // Continue until no more packets can be processed
            
            // Always ACK the EOF packet with the expectedseq value
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expectedseq;  // Use the next expected sequence number
            sndpkt->hdr.ctr_flags = FIN;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            printf("EOF ACK sent: %d\n", sndpkt->hdr.ackno);
            eof_received = 1;
            
            // Don't break yet - wait for potential retransmissions
            if (eof_received) {
                // After a short delay, close the file and exit
                struct timeval wait_time;
                wait_time.tv_sec = 1;  // Wait for 1 second
                wait_time.tv_usec = 0;
                
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(sockfd, &readfds);
                
                // Wait for more packets or timeout
                int ready = select(sockfd + 1, &readfds, NULL, NULL, &wait_time);
                
                if (ready <= 0) {
                    // Timeout occurred, no more packets
                    printf("No more packets received, closing file\n");
                    
                    // Clean up the buffer before exiting
                    for (int i = 0; i < BUFFER_SIZE; i++) {
                        if (buffer_used[i] && packet_buffer[i] != NULL) {
                            free(packet_buffer[i]);
                        }
                    }
                    
                    fclose(fp);
                    break;
                }
                
                // If we got here, there's another packet to process
                // Continue the loop and process it
                continue;
            }
        }
        
        // Regular data packet handling
        gettimeofday(&tp, NULL);
        printf("Packet received - seqno: %d, expected: %d, datasize: %d\n", 
               recvpkt->hdr.seqno, expectedseq, recvpkt->hdr.data_size);
        
        if (expectedseq == recvpkt->hdr.seqno) {
            // Packet is in order
            VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
            
            // Write data to file
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            fflush(fp);
            
            // Create and send ACK
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            sndpkt->hdr.ctr_flags = ACK;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            // Update expected sequence and last ACK sent
            last_ack_sent = sndpkt->hdr.ackno;
            expectedseq += recvpkt->hdr.data_size;
            
            printf("ACK sent: %d (updated expected to %d)\n", sndpkt->hdr.ackno, expectedseq);
            
            // Process any packets in the buffer that can now be processed
            int processed;
            do {
                processed = 0;
                for (int i = 0; i < BUFFER_SIZE; i++) {
                    if (buffer_used[i] && buffer_seqno[i] == expectedseq) {
                        // Found a packet that can now be processed
                        tcp_packet *pkt = packet_buffer[i];
                        
                        // Write data to file
                        fseek(fp, pkt->hdr.seqno, SEEK_SET);
                        fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
                        fflush(fp);
                        
                        printf("Processed buffered packet with seqno %d\n", pkt->hdr.seqno);
                        
                        // Update expected sequence number
                        expectedseq += pkt->hdr.data_size;
                        
                        // Free buffer slot
                        free(packet_buffer[i]);
                        packet_buffer[i] = NULL;
                        buffer_seqno[i] = -1;
                        buffer_used[i] = 0;
                        
                        processed = 1;
                        break;
                    }
                }
            } while (processed); // Continue until no more packets can be processed
            
            // Send ACK for the latest data we've processed
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expectedseq;
            sndpkt->hdr.ctr_flags = ACK;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            last_ack_sent = sndpkt->hdr.ackno;
            printf("Updated ACK sent after processing buffer: %d\n", sndpkt->hdr.ackno);
            
        } else if (recvpkt->hdr.seqno > expectedseq) {
            // Packet is out of order - buffer it
            printf("Out of order packet (seqno: %d, expected: %d) - buffering\n", 
                   recvpkt->hdr.seqno, expectedseq);
            
            // Find an empty slot in the buffer
            int slot = -1;
            for (int i = 0; i < BUFFER_SIZE; i++) {
                if (!buffer_used[i]) {
                    slot = i;
                    break;
                }
            }
            
            if (slot != -1) {
                // We found an empty slot, buffer the packet
                int size = TCP_HDR_SIZE + recvpkt->hdr.data_size;
                packet_buffer[slot] = (tcp_packet *)malloc(size);
                
                if (packet_buffer[slot] != NULL) {
                    memcpy(packet_buffer[slot], recvpkt, size);
                    buffer_seqno[slot] = recvpkt->hdr.seqno;
                    buffer_used[slot] = 1;
                    printf("Buffered packet with seqno %d in slot %d\n", recvpkt->hdr.seqno, slot);
                } else {
                    printf("Failed to allocate memory for buffering packet\n");
                }
            } else {
                printf("Buffer full, dropping out-of-order packet\n");
            }
            
            // Send duplicate ACK for the packet we're expecting
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expectedseq;
            sndpkt->hdr.ctr_flags = ACK;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            printf("Duplicate ACK sent: %d\n", sndpkt->hdr.ackno);
        } else {
            // This is a retransmission of a packet we've already processed
            printf("Received retransmission of already processed packet\n");
            
            // Just send ACK for the current expected sequence number
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expectedseq;
            sndpkt->hdr.ctr_flags = ACK;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            printf("ACK sent for retransmission: %d\n", sndpkt->hdr.ackno);
        }
    }

    return 0;
}