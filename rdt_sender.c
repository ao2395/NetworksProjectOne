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

#define STDIN_FD    0
#define RETRY  120 //millisecond
#define window_size 10
int next_seqno=0;
int send_base=0;
int packetnum=0;
tcp_packet* window[window_size];
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




void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        stop_timer();
        printf("oldestpacket being retrasmitted seqno: %d\n",  window[oldestpacket%window_size]->hdr.seqno);

        VLOG(INFO, "Timeout happend");
        if(sendto(sockfd, window[oldestpacket%window_size], TCP_HDR_SIZE + get_data_size(window[oldestpacket%window_size]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
            
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

    while (packetnum<10){
        len = fread(buffer, 1, DATA_SIZE, fp);
        if ( len <= 0)
        {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            window[packetnum]=sndpkt;
            packetnum++;
            eof_sent=1;
            break;
        }
        send_base=next_seqno;
        next_seqno += len;
        last_data_byte=next_seqno;
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = send_base;
        window[packetnum]=sndpkt;
        packetnum++;
    }
    send_base=0;
    int packetseq=0;
    for (int i=0;i<packetnum;i++){
        VLOG(DEBUG, "Sending packet %d to %s", 
            packetseq, inet_ntoa(serveraddr.sin_addr));
            packetseq=packetseq+get_data_size(window[i]);
    /*
     * If the sendto is called for the first time, the system will
     * will assign a random port number so that server can send its
     * response to the src port.
     */
    if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }
    start_timer();
    }

    while (1)
    {
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0){
                    error("recvfrom");

                }
                recvpkt = (tcp_packet *)buffer;
                printf("ack received: %d\n", recvpkt->hdr.ackno);
                // printf("%d \n", get_data_size(recvpkt));
                assert(get_data_size(recvpkt) <= DATA_SIZE);
  if (recvpkt->hdr.ackno > send_base) {
    stop_timer();
    for (int i = 0; i < 3; i++) {
        duplicatepackets[i] = -1;  // Clear the array
    }
    duplicatepackets[0] = recvpkt->hdr.ackno;  // Store the new ACK
    dupepacketindex = 1;
    
    // Update send_base to the new acknowledged position
    int old_base = send_base;
    send_base = recvpkt->hdr.ackno;
    
    fprintf(stdout, "ACK received: %d, updating send_base from %d to %d\n", 
           recvpkt->hdr.ackno, old_base, send_base);
    
    // Check if all data is acknowledged
    fprintf(stdout, "EOF: %d, send_base: %d, last_data_byte: %d\n", 
            eof_sent, send_base, last_data_byte);
            
    if (recvpkt->hdr.ackno >= last_data_byte && last_data_byte <= send_base) {
        if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
                }
        fprintf(stdout, "All data acknowledged. Clean exit.\n");
        return 1;
    }
    for (int i=0;i<window_size;i++){
        printf("%d,",window[i]->hdr.data_size);
    }
    printf("\n");
    // Only calculate and send new packets if we haven't reached EOF yet
    if (!eof_sent) {
        // Calculate how many new packets we can send to fill the window
        int packets_to_send = 0;
        int oldoldestpacket = oldestpacket;
        int temp_base = old_base;
        
        while (temp_base < send_base) {
            temp_base += get_data_size(window[oldoldestpacket % window_size]);
            oldoldestpacket++;
            packets_to_send++;
        }
        
        fprintf(stdout, "Sending %d new packets to fill window\n", packets_to_send);

        // Send new packets only if we haven't reached EOF
        for (int i=0; i<packets_to_send; i++) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                // End of file reached - create EOF packet but DON'T put it in the window
                fprintf(stdout, "EOF reached, sending EOF packet\n");
                eof_packet = make_packet(0);
                // if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                //     (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                //     error("sendto");
                // }
                
                // Mark EOF as sent
                eof_sent = 1;

                if (send_base < next_seqno) {
                    start_timer();
                }
                break;
                // Break out of the sending loop
            }
            // Update last_data_byte for normal packets
            printf("seqno: %d\n",next_seqno);
            printf("len: %d\n",len);
            
            
            // Create and send a new packet
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            // Store the packet in the window and send it
            window[oldestpacket % window_size] = sndpkt;
            
            if (len>0){
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                     (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            fprintf(stdout, "Sending data packet %d (size %d)\n", next_seqno, len);
        }
            oldestpacket++;
            next_seqno += len;
            last_data_byte = next_seqno;
            
            if (send_base < next_seqno) {
                start_timer();
            }
        }
    } 
    else{
        oldestpacket++;
        if (send_base < next_seqno) {
            start_timer();
        }
        
    }

}
else{
    duplicatepackets[dupepacketindex] = recvpkt->hdr.ackno;
    dupepacketindex = (dupepacketindex + 1) % 3;
    if (duplicatepackets[0] == duplicatepackets[1] && 
        duplicatepackets[1] == duplicatepackets[2] && 
        duplicatepackets[0] != -1) {
        
        fprintf(stdout, "Fast retransmit triggered for sequence %d\n", duplicatepackets[0]);
        
        // Find the packet to retransmit (should be the one with seqno equal to the duplicate ACK value)
        int packet_to_retransmit = -1;
        
        for (int i = 0; i < window_size; i++) {
            int idx = (oldestpacket + i) % window_size;
            if (window[idx] != NULL && window[idx]->hdr.seqno == duplicatepackets[0]) {
                packet_to_retransmit = idx;
                break;
            }
        }
        
        // Retransmit the packet
        if (packet_to_retransmit != -1) {
            fprintf(stdout, "Retransmitting packet with sequence %d\n", window[packet_to_retransmit]->hdr.seqno);
            
            if (sendto(sockfd, window[packet_to_retransmit], 
                      TCP_HDR_SIZE + get_data_size(window[packet_to_retransmit]), 0, 
                      (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        } else {
            fprintf(stdout, "Packet with sequence %d not found in window\n", duplicatepackets[0]);
        }
        
        // Reset after fast retransmit
        for (int i = 0; i < 3; i++) {
            duplicatepackets[i] = -1;
        }
        dupepacketindex = 0;
}
    }
}
return 1;
}



