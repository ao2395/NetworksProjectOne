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
#define RETRY  120 //millisecond
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


void resend_packets(int sig) //editted to resend the oldest unacknowldged packet
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happend");
        tcp_packet* oldest_packet = packet_window[send_base % window_size]; //using modulus for the circular/loop buffer to get the oldest unacknowledged packet from the window array defined earlier
        if(sendto(sockfd, oldest_packet, TCP_HDR_SIZE + get_data_size(oldest_packet), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
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

    while (1) {
        // Only try to send if window isn't full
        while(next_seqno < send_base + window_size) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
                free(sndpkt);  // Free EOF packet immediately since we don't store it
                sndpkt = NULL; //packet pointer set to null after freeing
                fclose(fp);    // Close the file
                return 0;      // Exit program after EOF
            }

            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            
            // Store packet in window buffer
            packet_window[next_seqno % window_size] = sndpkt;
            sndpkt = NULL; //after its stored in the window buffer set the pointer to null 
            
            tcp_packet* current_packet = packet_window[next_seqno % window_size]; //sending the next packet from the window bufffer
            VLOG(DEBUG, "Sending packet %d to %s", 
                    next_seqno, inet_ntoa(serveraddr.sin_addr));

            if(sendto(sockfd, current_packet, TCP_HDR_SIZE + get_data_size(current_packet), 0, 
                        (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }

            if(next_seqno == send_base) {
                start_timer();
            }

            next_seqno += len;
        }

        // Try to receive ACK
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0) {
            continue;  // No ACK received, continue sending if possible
        }

        recvpkt = (tcp_packet *)buffer;
        
        // Process ACK - move window forward
        if(recvpkt->hdr.ackno > send_base) {
            // Free acknowledged packets
            //but checking if null first
            while(send_base < recvpkt->hdr.ackno) {
                if(packet_window[send_base % window_size] != NULL) {
                    free(packet_window[send_base % window_size]);
                    packet_window[send_base % window_size] = NULL;  // Clear pointer after freeing
                }
                send_base++;
            }
            // Restart timer for remaining packets
            if(send_base < next_seqno) {
                stop_timer();
                start_timer();
            } else {
                stop_timer();
            }
        }
    }
return 0;
}  // End of main()