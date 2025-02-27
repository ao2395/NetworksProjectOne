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

#include"packet.h"
#include"common.h"


// function prototypes
void start_timer(void);
void stop_timer(void);
void resend_packets(int sig);
void init_timer(int delay, void (*sig_handler)(int));


#define STDIN_FD    0
#define RETRY  500 //millisecond
int next_seqno=0;
int send_base=0;
int window_size = 10; //window size changed to 10 packets
tcp_packet* packet_window[10]; //using an array to save the packets in the window
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
int previous_acks[3] = {-1, -1, -1}; 
int acknum = 0;
int packet_count = 0;
int eof_reached = 0;       // Flag to mark EOF reached
int eof_packet_sent = 0;   // Flag to mark EOF packet sent
int eof_acked = 0;         // Flag to mark EOF acknowledged
tcp_packet *eof_packet = NULL;  // Store the EOF packet

void resend_packets(int sig) //editted to resend the oldest unacknowldged packet
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happend");
        
        // Check if we need to resend the EOF packet
        if (eof_reached && eof_packet_sent && !eof_acked) {
            printf("Timeout - Resending EOF packet\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        } else {
            // Normal case - resend the oldest unacknowledged packet
            tcp_packet* oldest_packet = packet_window[send_base/1456 % window_size]; 
            if (oldest_packet != NULL) {
                printf("Timeout - Resending packet with seqno: %d\n", oldest_packet->hdr.seqno);
                if(sendto(sockfd, oldest_packet, TCP_HDR_SIZE + get_data_size(oldest_packet), 0, 
                        (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            } else {
                printf("Warning: No packet found at index %d to resend\n", send_base % window_size);
            }
        }
        
        start_timer(); //starting the timer directly after resendnig   
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
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
    for(int i = 0; i < window_size; i++) {//initializing packet_window to null
        packet_window[i] = NULL;
    }


    //Stop and wait protocol
    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    send_base = 0;  // Initialize send_base to 0
    int fileclosed = 0;
    
    while (1) {
        // If EOF has been acknowledged, exit cleanly
        if (eof_acked) {
            printf("EOF packet has been acknowledged. Exiting cleanly.\n");
            break;
        }
        
        // Only try to send if window isn't full and we haven't reached EOF
        while (next_seqno < send_base + window_size * DATA_SIZE && !eof_reached) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            
            if (len <= 0) {
                VLOG(INFO, "End Of File has been reached");
                printf("EOF detected - creating EOF packet\n");
                
                // Create the EOF packet
                eof_packet = make_packet(0);
                eof_reached = 1;
                fileclosed = 1;
                fclose(fp);
                
                // We don't send the EOF packet immediately
                // We'll send it after all data packets are acknowledged
                break;
            }
            
            // Create and store the packet
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            
            // Store in window
            int store_idx = (next_seqno / DATA_SIZE) % window_size;
            packet_window[store_idx] = sndpkt;
            
            // Log packet info
            VLOG(DEBUG, "Sending packet %d to %s", 
                next_seqno, inet_ntoa(serveraddr.sin_addr));
            
            // Send the packet
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            // Start timer if this is the first packet in the window
            if(next_seqno == send_base) {
                start_timer();
            }
            
            // Update sequence number and packet count
            next_seqno += len;
            packet_count++;
        }
        
        // If all data is acknowledged and we've reached EOF but haven't sent EOF packet yet
        if (eof_reached && !eof_packet_sent && send_base >= next_seqno) {
            printf("All data acknowledged, sending EOF packet\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            eof_packet_sent = 1;
            start_timer(); // Start timer for EOF packet
        }
        
        // Print window state for debugging
        // printf("Window state: ");
        // for (int i = 0; i < window_size; i++) {
        //     int idx = (send_base / DATA_SIZE + i) % window_size;
        //     printf("%d", packet_window[idx] != NULL ? 1 : 0);
        // }
        // printf(" (base: %d, next: %d)\n", send_base, next_seqno);
        // fflush(stdout);
        
        // Try to receive ACK with a timeout
        // fd_set readfds;
        // struct timeval tv;
        
        // FD_ZERO(&readfds);
        // FD_SET(sockfd, &readfds);
        
        // tv.tv_sec = 0;
        // tv.tv_usec = 100000; // 100ms timeout
        
        // int ready = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        
        // if (ready <= 0) {
        //     continue; // Timeout or error, continue the loop
        // }
        
        // ACK received
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0) {
            continue;
        }
        
        recvpkt = (tcp_packet *)buffer;
        printf("ACK RECEIVED: %d (send_base: %d) - %s\n", 
               recvpkt->hdr.ackno, 
               send_base,
               recvpkt->hdr.ackno <= send_base ? "DUPLICATE" : "NEW");
        fflush(stdout);
        
        // Check if this ACK is for the EOF packet
        if (eof_packet_sent && recvpkt->hdr.ackno >= next_seqno) {
            printf("Received ACK for EOF packet\n");
            eof_acked = 1;
            stop_timer();
            continue;
        }
        
        if(recvpkt->hdr.ackno > send_base) {
            // ACK for new data - reset duplicate counter
            previous_acks[0] = previous_acks[1] = previous_acks[2] = -1;
            acknum = 0;
            
            // Free acknowledged packets and update send_base
            while(send_base < recvpkt->hdr.ackno) {
                int idx = (send_base / DATA_SIZE) % window_size;
                if(packet_window[idx] != NULL) {
                    free(packet_window[idx]);
                    packet_window[idx] = NULL;
                }
                
                // Increment by actual packet size or ACK value
                if (send_base + DATA_SIZE <= recvpkt->hdr.ackno) {
                    send_base += DATA_SIZE; // Full packet
                } else {
                    send_base = recvpkt->hdr.ackno; // Partial packet or exact match
                }
            }
            
            // Handle timer
            if(send_base < next_seqno) {
                stop_timer();
                start_timer();
            } else if (!eof_packet_sent && eof_reached) {
                // If all data is acked and we have EOF, prepare to send EOF
                stop_timer();
            } else {
                stop_timer();
            }
        } else {
            // This might be a duplicate ACK
            VLOG(INFO, "Duplicate ACK received");
            previous_acks[acknum % 3] = recvpkt->hdr.ackno;
            acknum++;
            
            // Check for triple duplicate ACKs
            if (acknum >= 3 && 
                previous_acks[0] == previous_acks[1] && 
                previous_acks[1] == previous_acks[2] && 
                previous_acks[0] != -1) {
                
                VLOG(INFO, "3 Duplicate ACKs detected - Fast retransmit");
                
                // Fast retransmit the missing packet
                int idx = (send_base / DATA_SIZE) % window_size;
                tcp_packet* retransmit_packet = packet_window[idx];
                
                if (retransmit_packet != NULL) {
                    printf("Fast retransmitting packet with seqno: %d\n", retransmit_packet->hdr.seqno);
                    if(sendto(sockfd, retransmit_packet, TCP_HDR_SIZE + get_data_size(retransmit_packet), 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                    // Reset duplicate ACK counter
                    previous_acks[0] = previous_acks[1] = previous_acks[2] = -1;
                    acknum = 0;
                } else {
                    printf("Warning: No packet found at index %d for fast retransmit\n", idx);
                }
            }
        }
    }
    
    return 0;
}