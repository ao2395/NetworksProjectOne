#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <limits.h>  // For INT_MAX
#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond
#define window_size 10
int next_seqno=0;
int send_base=0;
int packetnum=0;
tcp_packet* window[window_size];
int packet_seqnos[window_size]; // To keep track of sequence numbers in the window
int window_count = 0; // Count of active packets in the window
int duplicatepackets[3]={-1,-1,-1};
int dupepacketindex=0;
int eof_sent = 0;
int last_data_byte = 0;
int sockfd, serverlen;
int oldestpacket=0;
int packetseq=0;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
tcp_packet *eof_packet = NULL;

enum {
    SENDING_DATA,
    WAITING_FOR_EOF_ACK,
    TERMINATED
} sender_state = SENDING_DATA;

// Forward declaration
void cleanup_resources();
int find_oldest_unacked_packet();

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void resend_packets(int sig) {
    if (sig == SIGALRM) {
        stop_timer();
        
        // Check if we're waiting for EOF acknowledgment
        if (eof_sent && sender_state == WAITING_FOR_EOF_ACK) {
            fprintf(stdout, "Retransmitting EOF packet\n");
            // Ensure flag is set for EOF packet
            eof_packet->hdr.ctr_flags = FIN;
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        } else {
            // Find the oldest unacknowledged packet
            int oldest_idx = find_oldest_unacked_packet();
            
            if (oldest_idx != -1) {
                oldestpacket = oldest_idx;  // Update the oldestpacket tracker
                fprintf(stdout, "Retransmitting packet with seqno: %d\n", 
                       window[oldest_idx]->hdr.seqno);
                VLOG(INFO, "Timeout happened");
                
                // Actually send the packet
                if(sendto(sockfd, window[oldest_idx], 
                         TCP_HDR_SIZE + get_data_size(window[oldest_idx]), 0, 
                         (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            } else {
                fprintf(stdout, "No unacknowledged packets in window\n");
            }
        }
        
        start_timer();
    }
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void cleanup_resources() {
    // Free any allocated window packets
    for (int i = 0; i < window_size; i++) {
        if (window[i] != NULL) {
            free(window[i]);
            window[i] = NULL;
        }
    }
    
    // Free EOF packet if it exists
    if (eof_packet != NULL) {
        free(eof_packet);
        eof_packet = NULL;
    }
}

int find_oldest_unacked_packet() {
    int oldest_idx = -1;
    int oldest_seqno = INT_MAX;
    
    for (int i = 0; i < window_size; i++) {
        if (window[i] != NULL && window[i]->hdr.seqno >= send_base && window[i]->hdr.seqno < oldest_seqno) {
            oldest_seqno = window[i]->hdr.seqno;
            oldest_idx = i;
        }
    }
    
    return oldest_idx;
}


int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol
    init_timer(RETRY, resend_packets);

    // Initialize the window with initial packets
    while (packetnum < window_size) {
        len = fread(buffer, 1, DATA_SIZE, fp);
        if (len <= 0) {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            eof_packet = sndpkt;
            window[packetnum] = sndpkt;
            sndpkt->hdr.seqno = next_seqno;  // Set the correct sequence number for EOF
            packet_seqnos[packetnum] = next_seqno;
                        
            // Set header flags to indicate EOF
            sndpkt->hdr.ctr_flags = FIN;  // Mark as EOF packet
            packetnum++;
            window_count++;
            eof_sent = 1;
            sender_state = WAITING_FOR_EOF_ACK;
            break;
        }
        
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = next_seqno;
        window[packetnum] = sndpkt;
        packet_seqnos[packetnum] = next_seqno; // Store sequence number
        
        next_seqno += len;
        last_data_byte = next_seqno;
        packetnum++;
        window_count++;
    }
    send_base = 0;

    // Send initial packets
    for (int i = 0; i < packetnum; i++) {
        VLOG(DEBUG, "Sending packet %d to %s", 
            window[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        
        if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                  (const struct sockaddr *)&serveraddr, serverlen) < 0) {
            error("sendto");
        }
    }
    // Start timer after sending all initial packets
    if (packetnum > 0) {
        start_timer();
    }

    while (1)
    {
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0){
            error("recvfrom");
        }
        
        recvpkt = (tcp_packet *)buffer;
        printf("ack received: %d\n", recvpkt->hdr.ackno);
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        
        if (recvpkt->hdr.ackno > send_base) {
            int old_send_base = send_base; // Store the old send_base value
            stop_timer();
            
            // Clear duplicate ACK array
            for (int i = 0; i < 3; i++) {
                duplicatepackets[i] = -1;
            }
            duplicatepackets[0] = recvpkt->hdr.ackno;
            dupepacketindex = 1;
            
            // Update send_base to the new acknowledged position
            send_base = recvpkt->hdr.ackno;
            
            fprintf(stdout, "ACK received: %d, updating send_base from %d to %d\n", 
                   recvpkt->hdr.ackno, old_send_base, send_base);
            
            // Calculate number of packets that have been fully acknowledged
            int freed_slots = 0;
            for (int i = 0; i < window_size; i++) {
                if (window[i] != NULL && window[i]->hdr.seqno + get_data_size(window[i]) <= send_base) {
                    // This packet is fully acknowledged
                    fprintf(stdout, "Packet with seqno %d is fully acknowledged\n", window[i]->hdr.seqno);
                    window[i] = NULL; // Free the slot
                    freed_slots++;
                    window_count--;
                }
            }
            
            // Check if all data is acknowledged
            fprintf(stdout, "EOF: %d, send_base: %d, last_data_byte: %d\n", 
                    eof_sent, send_base, last_data_byte);
                    
            if (eof_sent && send_base >= last_data_byte) {
                sender_state = TERMINATED;
                fprintf(stdout, "All data acknowledged including EOF. Clean exit.\n");
                
                // Send final acknowledgment to ensure receiver knows we're done
                if (eof_packet != NULL) {
                    // Send one last time to ensure receipt
                    if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                    fprintf(stdout, "Sent final EOF confirmation\n");
                }
                
                stop_timer(); // Important to stop the timer before exiting
                cleanup_resources();
                return 1;
            }
            
            // Only read and send new packets if we haven't reached EOF yet
            if (!eof_sent) {
                // Send new packets to fill the freed slots
                for (int i = 0; i < freed_slots; i++) {
                    // Find a free slot in the window
                    int free_slot = -1;
                    for (int j = 0; j < window_size; j++) {
                        if (window[j] == NULL) {
                            free_slot = j;
                            break;
                        }
                    }
                    
                    // If we found a free slot, read a new packet and send it
                    if (free_slot != -1) {
                        len = fread(buffer, 1, DATA_SIZE, fp);
                        if (len <= 0) {
                            // End of file reached - create EOF packet
                            fprintf(stdout, "EOF reached, sending EOF packet\n");
                            eof_packet = make_packet(0);
                            eof_packet->hdr.seqno = next_seqno;  // Set the correct sequence number for EOF
                            eof_packet->hdr.ctr_flags = FIN;  // Mark as EOF
                            
                            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                                (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                                error("sendto");
                            }
                            
                            // Mark EOF as sent
                            eof_sent = 1;
                            last_data_byte = next_seqno;  // Set last_data_byte to include the EOF packet
                            sender_state = WAITING_FOR_EOF_ACK;
                            break;
                        }
                        
                        // Create and send a new packet
                        sndpkt = make_packet(len);
                        memcpy(sndpkt->data, buffer, len);
                        sndpkt->hdr.seqno = next_seqno;
                        
                        // Store the packet in the window and send it
                        window[free_slot] = sndpkt;
                        window_count++;
                        
                        fprintf(stdout, "Sending data packet %d (size %d) in slot %d\n", 
                                next_seqno, len, free_slot);
                        
                        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                                 (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                            error("sendto");
                        }
                        
                        next_seqno += len;
                        last_data_byte = next_seqno;
                    }
                }
            }
            
            // Restart timer if there are unacknowledged packets
            if (send_base < next_seqno) {
                start_timer();
            }
        } else {
            duplicatepackets[dupepacketindex] = recvpkt->hdr.ackno;
            dupepacketindex = (dupepacketindex + 1) % 3;
            if (duplicatepackets[0] == duplicatepackets[1] && 
                duplicatepackets[1] == duplicatepackets[2] && 
                duplicatepackets[0] != -1) {
                
                fprintf(stdout, "Fast retransmit triggered for ACK sequence %d\n", duplicatepackets[0]);
                
                // The ACK value indicates the next expected byte, so we need to find the packet 
                // whose seqno + data_size would reach this ACK value
                int packet_to_retransmit = -1;
                int expected_seqno = duplicatepackets[0];
                
                // First look for an exact match where a packet's end boundary equals the duplicate ACK
                for (int i = 0; i < window_size; i++) {
                    if (window[i] != NULL) {
                        int packet_end = window[i]->hdr.seqno + get_data_size(window[i]);
                        
                        // This packet's end boundary matches the duplicate ACK value
                        if (packet_end == expected_seqno) {
                            packet_to_retransmit = i;
                            fprintf(stdout, "Found exact packet match for duplicate ACK %d\n", expected_seqno);
                            break;
                        }
                    }
                }
                
                // If no exact match found, look for the first packet beyond the current ACK point
                if (packet_to_retransmit == -1) {
                    int lowest_seqno = INT_MAX;
                    
                    for (int i = 0; i < window_size; i++) {
                        if (window[i] != NULL && window[i]->hdr.seqno >= send_base) {
                            if (window[i]->hdr.seqno < lowest_seqno) {
                                lowest_seqno = window[i]->hdr.seqno;
                                packet_to_retransmit = i;
                            }
                        }
                    }
                }
                
                // Retransmit the identified packet
                if (packet_to_retransmit != -1) {
                    fprintf(stdout, "Fast retransmitting packet with sequence %d\n", 
                            window[packet_to_retransmit]->hdr.seqno);
                    
                    if (sendto(sockfd, window[packet_to_retransmit], 
                            TCP_HDR_SIZE + get_data_size(window[packet_to_retransmit]), 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                }
                
                // Reset after fast retransmit
                for (int i = 0; i < 3; i++) {
                    duplicatepackets[i] = -1;
                }
                dupepacketindex = 0;
            }
        }
    } // End of while(1) loop
    
    cleanup_resources();
    return 1;
} // End of main function