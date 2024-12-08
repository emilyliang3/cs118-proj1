#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>

#define MAX_WINDOW_SIZE 20
#define HEADER_LEN 12
#define MSS 1012 // MSS = Maximum Segment Size (aka max length)
typedef struct {
	uint32_t ack;
	uint32_t seq;
	uint16_t length;
	uint8_t flags;
	uint8_t unused;
	uint8_t payload[MSS];
} packet;

void add_pkt_to_buff(packet buff[MAX_WINDOW_SIZE], int *buff_count, packet pkt) {
   if (*buff_count < MAX_WINDOW_SIZE) {
      buff[(*buff_count)++] = pkt;
   } else {
      fprintf(stderr, "Buffer full- cannot add more packets!\n");
   }
   fprintf(stderr, "\tBuffer contents: ");
   for (int i = 0 ; i < *buff_count; i++) {
      fprintf(stderr, "%d ", ntohl(buff[i].seq));
   }
   fprintf(stderr, "\n");
}

void recv_packet(packet recv_packets[MAX_WINDOW_SIZE], int *recv_count, packet sent_packets[MAX_WINDOW_SIZE], int *sent_count, packet pkt, uint32_t *exp_seq) {
   // Process ack
   if ((pkt.flags >> 1) & 1) {
      for (int i = 0; i < *sent_count; i++) {
         // Remove packets whose seq # < received ack
         if (ntohl(sent_packets[i].seq) < ntohl(pkt.ack)) {
            fprintf(stderr, "Removing packet %d\n", ntohl(sent_packets[i].seq));
            for (int j = i; j < *sent_count-1; j++) {
               sent_packets[j] = sent_packets[j+1];
            }
            (*sent_count)--;
            i--; // so that we check the packet that replaces the removed one
         }
      }
   }

   // Add packet to received buffer (only if there is a payload)
   if (ntohs(pkt.length) == 0) {
      return;
   }
   // Do not add packets that are duplicates of previously received packets
   if (ntohl(pkt.seq) >= *exp_seq) 
      add_pkt_to_buff(recv_packets, recv_count, pkt); 
   // Look for packet that matches expected seq num to print out
   int packet_found;
   do {
      packet_found = 0;
      for (int i = 0; i < *recv_count; i++) {
         if (ntohl(recv_packets[i].seq) == *exp_seq) {
               packet_found = 1;
               write(1, recv_packets[i].payload, ntohs(recv_packets[i].length)); // Print out found packet
               *exp_seq += ntohs(recv_packets[i].length);
               // Remove packet from buffer
               for (int j = i; j < *recv_count - 1; j++) {
                  recv_packets[j] = recv_packets[j + 1];
               }
               (*recv_count)--;
               break;  // Restart loop to check for the new expected seq num
         }
      }
   } while (packet_found);
}

// Returns index of packet with lowest seq num; returns -1 if buffer is empty
int get_lowest_pkt(packet sent_packets[MAX_WINDOW_SIZE], int sent_count) {
   int idx = -1;
   int lowest_seq = 999999999;
   for (int i = 0; i < sent_count; i++) {
      if (ntohl(sent_packets[i].seq) < lowest_seq) {
         lowest_seq = ntohl(sent_packets[i].seq);
         idx = i;
      }
   }
   return idx;
}

int main(int argc, char *argv[]) {
   // Expects hostname and port arguments
   if (argc < 3) {
      fprintf(stderr, "Expected 2 arguments, got less than 2.");
      return -1;
   }

   // Create socket
   int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
   if (sockfd < 0) fprintf(stderr, "Error creating socket.\n");
   // Make non blocking:
   int flags = fcntl(sockfd, F_GETFL, 0);
   if (flags < 0) {
      fprintf(stderr, "Error retrieving sockfd flags.\n");
   }
   flags |= O_NONBLOCK;
   if (fcntl(sockfd, F_SETFL, flags) < 0) {
      fprintf(stderr, "Error setting sockfd to non-blocking.\n");
   }

   // Construct server address
   struct sockaddr_in serveraddr;
   serveraddr.sin_family = AF_INET; // use IPv4
   char *IP_ADDRESS = argv[1];
   if (strcmp(argv[1], "localhost") == 0) {
      IP_ADDRESS = "127.0.0.1";
   }
   fprintf(stderr, "Server IP: %s\n", IP_ADDRESS);
   serveraddr.sin_addr.s_addr = inet_addr(IP_ADDRESS);
   // Set sending port
   int PORT;
   if (sscanf(argv[2], "%d", &PORT) < 1) {
      fprintf(stderr, "Error getting port number from command line arguments.");
      PORT = 8080;
   }
   fprintf(stderr, "Server Port: %d\n", PORT);
   serveraddr.sin_port = htons(PORT); // Big endian

   // Make stdin non-blocking
   flags = fcntl(STDIN_FILENO, F_GETFL, 0);
   if (flags == -1) {
      fprintf(stderr, "Error getting stdin flags.\n");
   }
   flags |= O_NONBLOCK;
   if (fcntl(STDIN_FILENO, F_SETFL, flags) == -1) {
      fprintf(stderr, "Error setting stdin to non-blocking.\n");
   }

   // Receiving buffer to store incoming data
   packet recv_packets[MAX_WINDOW_SIZE];
   int recv_count = 0;
   uint32_t next_exp_seq = 0;
   socklen_t serversize = sizeof(serveraddr); // Temp buffer for recvfrom API

   // Retransmission
   uint32_t most_recent_ack = 0;
   int num_duplicate_acks = 0;
   time_t ack_time = time(NULL);

   // For handshake
   srand(time(NULL));
   int connected = 0; 
   uint32_t rand_seq = 0;

   // Sending buffer to store sent packets and buffer for data from stdin
   packet sent_packets[MAX_WINDOW_SIZE];
   int sent_count = 0;
   int bytes_read; 
   char stdin_buf[MSS];
   uint32_t current_seq = 0;

   while(1) {
      // Initiate three way handshake
      if (connected == 0) {
         rand_seq = (uint32_t)(rand()) >> 1; // ensure rand seq number is less than half of uint32_max
         packet hs_pkt = {
            .ack = htonl(0),
            .seq = htonl(rand_seq),
            .length = htons(0),
            .flags = 0b00000001,
            .unused = 0,
            .payload = {0}
         };
         int did_send = sendto(sockfd, &hs_pkt, HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
         fprintf(stderr, "Sent first handshake packet- SEQ=%d.\n", rand_seq);
         packet rec_hs_pkt = {0};
         time_t time_now = time(NULL);
         while (time(NULL) - time_now < 1) {
            int bytes_recvd = recvfrom(sockfd, &rec_hs_pkt, sizeof(rec_hs_pkt), 0, (struct sockaddr*) &serveraddr, &serversize);
            if (bytes_recvd >= HEADER_LEN) {
               fprintf(stderr, "Received second handshake packet- SEQ=%d, ACK=%d.\n", ntohl(rec_hs_pkt.seq), ntohl(rec_hs_pkt.ack));
               uint32_t ack_num = ntohl(rec_hs_pkt.ack);
               uint32_t seq = ntohl(rec_hs_pkt.seq);
               uint16_t len = ntohs(rec_hs_pkt.length);
               bool syn = rec_hs_pkt.flags & 1;
               bool ack = (rec_hs_pkt.flags >> 1) & 1;
               // Verify packet
               if (syn && ack && ack_num == rand_seq+1) {
                  // fprintf(stderr, "Verified second handshake packet.\n");
                  packet hs_pkt2 = {
                     .ack = htonl(seq+1),
                     .seq = htonl(rand_seq+1),
                     .length = htons(0),
                     .flags = 0b00000010,
                     .unused = 0,
                     .payload = {0}
                  };
                  current_seq = rand_seq+1;
                  next_exp_seq = seq+1;
                  int did_send = sendto(sockfd, &hs_pkt2, HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
                  fprintf(stderr, "Sent third handshake packet- SEQ=%d, ACK=%d.\n", rand_seq+1, seq+1);
                  connected = 1;
                  current_seq++;
                  ack_time = time(NULL);
                  break;
               }
            }
         }
      }
      if (connected == 1) {
         // Listen for response from server 
         packet pkt = {0};
         int bytes_recvd = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr*) &serveraddr, &serversize);
         if (bytes_recvd == 0) fprintf(stderr, "Recevied zero bytes\n");
         int send_ack = 0; // If new packet is received this guarantees ack is sent even if no data is sent
         if (bytes_recvd >= HEADER_LEN) {
            fprintf(stderr, "Received packet- SEQ=%d, ACK=%d.\n", ntohl(pkt.seq), ntohl(pkt.ack));
            recv_packet(recv_packets, &recv_count, sent_packets, &sent_count, pkt, &next_exp_seq);
            if (bytes_recvd > HEADER_LEN) send_ack = 1;
            // Check 1 second timeout
            if ((pkt.flags >> 1) & 1) {
               ack_time = time(NULL);
               // Check for duplicate acks
               if (ntohl(pkt.ack) == most_recent_ack) {
                  num_duplicate_acks++;
                  if (num_duplicate_acks >= 3) {
                     int lowest_idx = get_lowest_pkt(sent_packets, sent_count);
                     if (lowest_idx >= 0) {
                        packet lowest_packet = sent_packets[lowest_idx];
                        int did_send = sendto(sockfd, &lowest_packet, ntohs(lowest_packet.length) + HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
                        fprintf(stderr, "Retransmitting packet %d b/c received duplicate acks.\n", ntohl(lowest_packet.seq));
                     }
                  }
               } else {
                  most_recent_ack = ntohl(pkt.ack);
               }
            }
         }
         // Retransmit if 1 second timer expires
         if (time(NULL) - ack_time >= 1) {
            int lowest_idx = get_lowest_pkt(sent_packets, sent_count);
            if (lowest_idx >= 0) {
               packet lowest_packet = sent_packets[lowest_idx];
               int did_send = sendto(sockfd, &lowest_packet, ntohs(lowest_packet.length) + HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
               fprintf(stderr, "Retransmitting packet %d b/c timer expired, sent %d characters.\n", ntohl(lowest_packet.seq), did_send);
            }
            ack_time = time(NULL);
         }

         // Only read data from stdin if there is space in sent buffer
         if (sent_count < MAX_WINDOW_SIZE) {
            bytes_read = read(STDIN_FILENO, stdin_buf, sizeof(stdin_buf));
            if (bytes_read > 0) {
               // fprintf(stderr, "Got input from stdin.\n");
               packet pkt = {
                  .ack = htonl(0),
                  .seq = htonl(current_seq),
                  .length = htons(bytes_read),
                  .flags = 0,
               };
               memcpy(pkt.payload, stdin_buf, sizeof(pkt.payload));
               // Add version of packet without ack
               fprintf(stderr, "\tAdding packet to send buffer...\n");
               add_pkt_to_buff(sent_packets, &sent_count, pkt);
               if (send_ack == 1) {
                  pkt.ack = htonl(next_exp_seq);
                  pkt.flags = 0b00000010;
                  send_ack = 0;
               }
               int did_send = sendto(sockfd, &pkt, bytes_read + HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
               fprintf(stderr, "Sent %d characters- SEQ=%d, ACK=%d.\n", did_send, current_seq, ntohl(pkt.ack));
               current_seq += bytes_read;
            }
         }
         if (send_ack == 1) {
            packet pkt = {
               .ack = htonl(next_exp_seq),
               .seq = htonl(0),
               .length = htons(0),
               .flags = 0b00000010,
               .payload = {0}
            };
            int did_send = sendto(sockfd, &pkt, HEADER_LEN, 0, (struct sockaddr*) &serveraddr, sizeof(serveraddr));
            fprintf(stderr, "ACK=%d\n", next_exp_seq);
         }
      }
   }

   close(sockfd);
   return 0;
}
