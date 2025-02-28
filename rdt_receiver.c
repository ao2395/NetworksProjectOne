
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

#define BUFFER_SIZE 20 //defining the size of the packet buffer for storing the out of order packets

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt; //pointer that holds the most recently received packet from the client
tcp_packet *sndpkt; //pointer that is used to make and send ACK packets 
int expectedseq = 0; //variable to track the next sequence number the receiver expects to receive from the sender, starts at 0 
int last_ack_sent = 0;  //variable that stores the ACK number from the last ACK packet that the client recieved  

tcp_packet* packet_buffer[BUFFER_SIZE]; // stroing the out of order packetss (type array)
int buffer_seqno[BUFFER_SIZE]; // the sequence number associated to the buffered packet
int buffer_used[BUFFER_SIZE]; // checks whether or not the buffer slot is currently taken (0 meants not in use 1 means in use)

int main(int argc, char **argv) { 
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp; //poiinter to a FILE structure to write received data into a file
    char buffer[MSS_SIZE]; //declaring an array of characters of size MSS(Max segment size)
    struct timeval tp; //struct to store the time values, when timestamp logging 

    /* 
     * check command line arguments 
     */
    if (argc != 3) { //first checking if we got exactly three command line arguments (prog name+port number+output file)
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1); //if not print a usage message and error code exit
    }
    portno = atoi(argv[1]); //converting the 2nd argument which is the port number from string type to int

    fp = fopen(argv[2], "w");  //opens the file with w to create an empty file or overwrite an existing one
    if (fp == NULL) { //checking if the file opening was succesful if not an error function is called 
        error(argv[2]); 
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0); //creating a new socket 
    if (sockfd < 0)  //if a value less than zero is returned then there was an error 
        error("ERROR opening socket"); //error function being called

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1; //this wil be used as a boolean flag when it come to socket options 
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int)); //setting the options for the socket 
//arguments being sockfd(file descriptor) SOL_SOCKET (defines the option at socket level) SO_REUSEADDR(specific option to set allowing the socket to be connected to an adrress in use) (const void *)&optval(pointer poiting to the option value) sizeof(int) (the size of the option value in bytes)
    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr)); //calling the fill serveraddr struct with zeros
    serveraddr.sin_family = AF_INET; //setting server address's address family field to AF_INET
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); //setting the IP address to INADDR_ANY which allows the server to accept connection on any network interface of the machine , then htonl will convert the value from host byte to network byte order 
    serveraddr.sin_port = htons((unsigned short)portno); //setting the port num field to the value sorted in portno, and also casts the portno to an unsigned short

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding"); //calling bind to connect the specified address and port

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number"); //logging a debug message using VLOG macro 

    clientlen = sizeof(clientaddr);//cleint wit size of the clientaddr, which we will then use in recvfrom() and sendto() calls
    int eof_received = 0; // declare and initialize a new int variable to keep track of when the EOF is received 
    
    
    for (int i = 0; i < BUFFER_SIZE; i++) { //initializing an array that we will use to buffer the out of order packets 
        packet_buffer[i] = NULL; //setting each pointer in hte packet buffer array to NULL
        buffer_seqno[i] = -1; // setting the sequence numbers to -1 to show that no packet is being stored yet
        buffer_used[i] = 0; // sets each "used" flag to 0 to show that the slot is available.
    }
    
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
//callling recvfrom() to receiver a UDP datagram by passing the socket descriptor, buffer MSS size, no special flags, the senders address and the size of the address structure
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, 
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom"); // returning a value less than 0 means error so it calls the error function
        }
        // below two lines: casting the received data in buffer to a tcp_packet struct, thenverifying that th data size reported in the packet is valid 
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        

        if (recvpkt->hdr.data_size == 0) { //to handle EOF, we check if the recieved packet is an EOF packet, we do this through looking at the data size, 0 indicating EOF
            VLOG(INFO, "End Of File packet received");
            //do while loop so we can process buffered packets that can now be handled 
            int processed;
            do {
                processed = 0; // here we aere using this variable so we can keep track of the packet that was processed in the current interation 
                for (int i = 0; i < BUFFER_SIZE; i++) { //going over all the slots in the buffer
                    if (buffer_used[i] && buffer_seqno[i] == expectedseq) { // if statement to check if our current buffer slot is being used, and if the seq number matched the expected one 
                        
                        tcp_packet *pkt = packet_buffer[i]; //making a local pointer to easily access the buffered packet
                        fseek(fp, pkt->hdr.seqno, SEEK_SET); //setting the file pointer at the byte offset we got from the packet's sequence number 
                        fwrite(pkt->data, 1, pkt->hdr.data_size, fp);// writing thr data from the packet intot he file
                        fflush(fp);
                        
                        // printf("Processed buffered packet with seqno %d\n", pkt->hdr.seqno); 
                        expectedseq += pkt->hdr.data_size; //updating the expected sequence number by adding the size of the processed data

                        free(packet_buffer[i]); //freeing the slot that was for the packet and resetting the slot to show that its not in use any more 
                        packet_buffer[i] = NULL;
                        buffer_seqno[i] = -1;
                        buffer_used[i] = 0;
                        processed = 1; //setting our processed flag to show that the packet is processed now 
                        break; //breaking out of the for loop
                    }
                }
            } while (processed); //keep the loop going as long as there is at least one packet that was processed in the last iteration
            sndpkt = make_packet(0);  //making a new packet with no data 
            sndpkt->hdr.ackno = expectedseq;  //setting the ack number to the current expected sequence number
            sndpkt->hdr.ctr_flags = FIN; // setting the control flag to FIN to show that this is the last ACK
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) { //sending the ACK packet back to the client and checking for error
                error("ERROR in sendto");
            }
            
            // printf("EOF ACK sent: %d\n", sndpkt->hdr.ackno); // printint that the EOF ACK was sent for checking 
            eof_received = 1; //flag set to 1 to show received and acknowleged EOF ppacket 

            if (eof_received) { //handling EOF and retransmission in this if else statement 
                struct timeval wait_time; //below setting a one sec timeout to wait for more packets
                wait_time.tv_sec = 1; 
                wait_time.tv_usec = 0;
                fd_set readfds; //making a file descriptor 
                FD_ZERO(&readfds); //empty
                FD_SET(sockfd, &readfds); // add sockets to the set 
          
                int ready = select(sockfd + 1, &readfds, NULL, NULL, &wait_time); //wait for any activity on the socket or for the timeout to expire
                if (ready <= 0) { //monitor if new packets have been sent
                    // printf("No more packets received, closing file\n");
                    for (int i = 0; i < BUFFER_SIZE; i++) { //freeing any packets left in the buffer 
                        if (buffer_used[i] && packet_buffer[i] != NULL) {
                            free(packet_buffer[i]);
                        }
                    }
                    
                    fclose(fp); //closing the output file and breaking 
                    break; //breaking out of the main while loop
                }
                continue;
            }
        }
        gettimeofday(&tp, NULL); 
        // printf("Packet received - seqno: %d, expected: %d, datasize: %d\n", //printing all info in the received packet
            //    recvpkt->hdr.seqno, expectedseq, recvpkt->hdr.data_size);
        
        if (expectedseq == recvpkt->hdr.seqno) { //cheking if the packet that was received matches witht he expected sequence number
            VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno); //logging the debug info
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET); //file pointer is at the position of the byte offset 
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp); //writing the packet data into the file
            fflush(fp); //forcing the data to be written immediately 

            sndpkt = make_packet(0); //making a new packet to send ACKS
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size; //setting the ACK num to the next expected byte which is current sequence + data size
            sndpkt->hdr.ctr_flags = ACK; //setting thje control flag to show that we are dealing with an ACK package
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) { //checking for any errors while sending the ACK backet back to the client
                error("ERROR in sendto");
            }
            last_ack_sent = sndpkt->hdr.ackno; //updated to the last ACK sent
            expectedseq += recvpkt->hdr.data_size; //update the expected sequence number for the next packet 
            // printf("ACK sent: %d (updated expected to %d)\n", sndpkt->hdr.ackno, expectedseq); //logging info on ACK and the expcted sequence number 
            int processed; //starting a do while loop to process any buffered packets
            do {
                processed = 0; //flag set to 0 for now
                for (int i = 0; i < BUFFER_SIZE; i++) { ///looping through the slots in the buffer
                    if (buffer_used[i] && buffer_seqno[i] == expectedseq) { //checking the buffer slot's status to see if its in use, and also if the packet's sequence number matches the epxected one
                        tcp_packet *pkt = packet_buffer[i]; //accessing the reference to the buferred packet 
                        
                        fseek(fp, pkt->hdr.seqno, SEEK_SET); //writing the buffered packet's data into the file (next 3 lines)
                        fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
                        fflush(fp);
                        // printf("Processed buffered packet with seqno %d\n", pkt->hdr.seqno);
                        //updating the expected seq number for the next packet
                        expectedseq += pkt->hdr.data_size;

                        free(packet_buffer[i]); //freeing the buffer slot and setting its state to 0 to show that it is not in use
                        packet_buffer[i] = NULL;
                        buffer_seqno[i] = -1;
                        buffer_used[i] = 0;
                        
                        processed = 1;//setting a flag to show that the packet was processed 
                        break;
                    }
                }
            } while (processed); //as long as at least one packet was processed, continue the loop
        
            sndpkt = make_packet(0); //making a new ACK, with the new expected sequence number 
            sndpkt->hdr.ackno = expectedseq; //sending the dup ACK 
            sndpkt->hdr.ctr_flags = ACK;
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) { 
                error("ERROR in sendto");
            }
        
            last_ack_sent = sndpkt->hdr.ackno; //updating the tracking variable
            // printf("Updated ACK sent after processing buffer: %d\n", sndpkt->hdr.ackno); //logging that an updated ACK was sent
        } else if (recvpkt->hdr.seqno > expectedseq) { // else if section to handle the case when the received packet had a seq number > expected meaning its out of order
            // printf("Out of order packet (seqno: %d, expected: %d) - buffering\n", 
            //        recvpkt->hdr.seqno, expectedseq);
            int slot = -1; // setting the slot to -1 to show that the slot has not been found yet
            for (int i = 0; i < BUFFER_SIZE; i++) { //looping over the buffer so we can find the first slot that is empty 
                if (!buffer_used[i]) {
                    slot = i; //set slot to the found empty index
                    break; //break when its found 
                }
            }
            
            if (slot != -1) { // check if there is an unused slot in the buffer 
                int size = TCP_HDR_SIZE + recvpkt->hdr.data_size; //calcualting the total size for the packet
                packet_buffer[slot] = (tcp_packet *)malloc(size); //using malloc to allocate memory in that size for the packet in buffer
                
                if (packet_buffer[slot] != NULL) { //quick check to see if the memory allocation was done successfully 
                    memcpy(packet_buffer[slot], recvpkt, size);// if so then we copy the packet recieved to the buffer space
                    buffer_seqno[slot] = recvpkt->hdr.seqno; //stores its respective sequence number
                    buffer_used[slot] = 1; //marks the flag to 1 to show that the space is no longer empty 
                    // printf("Buffered packet with seqno %d in slot %d\n", recvpkt->hdr.seqno, slot);
                } else { //otherwise
                    // printf("Failed to allocate memory for buffering packet\n"); //we indicate that the allocation was failed
                }
            } else { // if there was no free buffer than log that the buffer is full and we will begin to have packet loss
                // printf("Buffer full, dropping out-of-order packet\n");
            }
            sndpkt = make_packet(0); //creating a new packet to send ACKS
            sndpkt->hdr.ackno = expectedseq; //set the ACK number to the whatever the expected seq number indicates, this was we can let the sender know that we still need the  expected seq numer 
            sndpkt->hdr.ctr_flags = ACK;  //settign the control flag to ACK to show that its an ACK packet 
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,  
                    (struct sockaddr *) &clientaddr, clientlen) < 0) { //checking for any errors in sending, here we are sending the duplicate ACK back tot he cleint 
                error("ERROR in sendto");
            }
            
            // printf("Duplicate ACK sent: %d\n", sndpkt->hdr.ackno);//loggint the sending of the duplicate ACK and showing its ack number
        } else { // this final else handles the case when the seq number is less than expected meaning that the packet we processed already is retransmitted
            // printf("Received retransmission of already processed packet\n");
            sndpkt = make_packet(0);  //making the ACK packet with the expected sequence number 
            sndpkt->hdr.ackno = expectedseq;
            sndpkt->hdr.ctr_flags = ACK; //setting flag to ACK
            
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) { //sendin the ACK packet back to the client
                error("ERROR in sendto");
            }
            // printf("ACK sent for retransmission: %d\n", sndpkt->hdr.ackno);
        }
    }

    return 0;
}
