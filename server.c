#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

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
         // Remove packets whose seq # < recevied ack
         if (ntohl(sent_packets[i].seq) < ntohl(pkt.ack)) {
            for (int j = i; j < *sent_count-1; j++) {
               sent_packets[j] = sent_packets[j+1];
            }
            (*sent_count)--;
            i--; // so that we check the packet that replaces the removed one
         }
      }
   }

   // Add packet to recevied buffer (only if there is a payload)
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
   // Expects port argument
   if (argc < 2) {
      fprintf(stderr, "Expected at least one argument, got none.");
      return -1;
   }

   // Set up socket
   int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
   int flags = fcntl(sockfd, F_GETFL, 0);
   if (flags < 0) {
      fprintf(stderr, "Error retrieving sockfd flags.\n");
   }
   flags |= O_NONBLOCK;
   if (fcntl(sockfd, F_SETFL, flags) < 0) {
      fprintf(stderr, "Error setting sockfd to non-blocking.\n");
   }
   struct sockaddr_in servaddr;
   servaddr.sin_family = AF_INET; // use IPv4
   servaddr.sin_addr.s_addr = INADDR_ANY; // accept all connections

   // Set receiving port
   int PORT;
   if (sscanf(argv[1], "%d", &PORT) < 1) {
      fprintf(stderr, "Error getting port number from command line arguments.\n");
      PORT = 8080;
   }
   else {
      fprintf(stderr, "Read port number %d\n", PORT);
   }
   servaddr.sin_port = htons(PORT); // Big endian

   int did_bind = bind(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr));
   if (did_bind < 0) {
      fprintf(stderr, "Failed to bind socket.\n");
      return errno;
   }

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
   struct sockaddr_in clientaddr; // Same information, but about client
   socklen_t clientsize = sizeof(clientaddr);

   // Retransmission
   uint32_t most_recent_ack = 0;
   int num_duplicate_acks = 0;
   time_t ack_time = time(NULL);

   // For handshake
   srand(time(NULL));
   int client_connected = 0;
   uint32_t rand_seq = 0;

   // Sending buffer to store sent packets and buffer for data from stdin
   packet sent_packets[MAX_WINDOW_SIZE];
   int sent_count = 0;
   int bytes_read; 
   char stdin_buf[MSS];
   uint32_t current_seq = 0;

   while(1) {
      packet pkt = {0};
      int bytes_recvd = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr*) &clientaddr, &clientsize);
      // Wait for three way handshake
      if (!client_connected) {
         if (bytes_recvd >= HEADER_LEN) {
            fprintf(stderr, "Received first handshake packet- SEQ=%d.\n", ntohl(pkt.seq));
            uint32_t rand_seq = (uint32_t)(rand()) >> 1; // ensure rand seq number is less than half of uint32_max
            uint32_t initial_seq = ntohl(pkt.seq);
            packet hs_pkt = {
               .ack = htonl(ntohl(pkt.seq)+1),
               .seq = htonl(rand_seq),
               .length = htons(0),
               .flags = 0b00000011,
               .unused = 0,
               .payload = {0}
            };
            int did_send = sendto(sockfd, &hs_pkt, HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
            fprintf(stderr, "Sent second handshake packet- SEQ=%d, ACK=%d.\n", rand_seq, ntohl(hs_pkt.ack));
            time_t time_now = time(NULL);
            while (time(NULL) - time_now < 1) {
               int bytes_recvd = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr*) &clientaddr, &clientsize);
               if (bytes_recvd >= HEADER_LEN) {
                  fprintf(stderr, "Received third handshake packet- SEQ=%d, ACK=%d.\n", ntohl(pkt.seq), ntohl(pkt.ack));
                  uint32_t ack_num = ntohl(pkt.ack);
                  uint32_t seq = ntohl(pkt.seq);
                  uint16_t len = ntohs(pkt.length);
                  bool syn = pkt.flags & 1;
                  bool ack = (pkt.flags >> 1) & 1;
                  // Verify packet
                  if (ack && ack_num == rand_seq+1 && seq == initial_seq+1) {
                     fprintf(stderr, "Verified third handshake packet- successfully connected to client.\n");
                     next_exp_seq = seq;
                     current_seq = ack_num;
                     client_connected = 1;
                     if (len==0) next_exp_seq++;
                     break;
                     // Just for this case ack the third packet sent in handshake
                     // packet ack_pkt = {
                     //    .ack = htonl(next_exp_seq+1),
                     //    .seq = htonl(0),
                     //    .length = htons(0),
                     //    .flags = 0b00000001,
                     //    .payload = {0}
                     // };
                     // int did_send = sendto(sockfd, &ack_pkt, HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
                  }
               }
            }
         }
         if (bytes_recvd < 0) continue;
      }

      if (client_connected) {
         /* 6. Inspect data from client */
         char* client_ip = inet_ntoa(clientaddr.sin_addr);
         int client_port = ntohs(clientaddr.sin_port); // Little endian

         // Process data
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
                        int did_send = sendto(sockfd, &lowest_packet, ntohs(lowest_packet.length) + HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
                        fprintf(stderr, "Retransmitting packet %d b/c received duplicate acks, sent %d characters.\n", ntohl(lowest_packet.seq), did_send);
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
               int did_send = sendto(sockfd, &lowest_packet, ntohs(lowest_packet.length) + HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
               fprintf(stderr, "Retransmitting packet %d b/c timer expired.\n", ntohl(lowest_packet.seq));
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
               int did_send = sendto(sockfd, &pkt, bytes_read + HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
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
            int did_send = sendto(sockfd, &pkt, HEADER_LEN, 0, (struct sockaddr*) &clientaddr, sizeof(clientaddr));
            fprintf(stderr, "ACK=%d\n", next_exp_seq);
         }
      }
   }
   // This will never be run:   
   close(sockfd);
   return 0;
}