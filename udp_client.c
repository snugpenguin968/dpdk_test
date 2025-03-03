/**
 * This UDP client sends a test message to the server,
 * waits for an echo, and then calculates the round-trip time (RTT).
 */
 #define _POSIX_C_SOURCE 200809L 
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <arpa/inet.h>
 #include <time.h> 
 #define SERVER_PORT 12345
 #define BUF_SIZE 1024
 
 double current_time_in_seconds(void);
 int main(void);
 
 // Returns the current time in seconds using a high-resolution clock
 double current_time_in_seconds(void) {
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return ts.tv_sec + ts.tv_nsec / 1e9;
 }
 
 int main(void) {
     int sockfd; 
     struct sockaddr_in servaddr; 
     char send_buffer[BUF_SIZE] = "Latency test message";
     char recv_buffer[BUF_SIZE];
     socklen_t servaddr_len = sizeof(servaddr);
 
     // Create UDP socket
     if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
         perror("socket creation failed");
         exit(EXIT_FAILURE);
     }
 
     // Set up server address structure
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family = AF_INET;
     servaddr.sin_port = htons(SERVER_PORT);
     if (inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr) <= 0) {
         perror("Invalid address or address not supported");
         close(sockfd);
         exit(EXIT_FAILURE);
     }
 
     // Begin latency measurement
     double start_time = current_time_in_seconds();
 
     if (sendto(sockfd, send_buffer, strlen(send_buffer), 0,
             (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
         perror("sendto failed");
         close(sockfd);
         exit(EXIT_FAILURE);
     }
 
     ssize_t recv_bytes = recvfrom(sockfd, recv_buffer, BUF_SIZE, 0,
                                 (struct sockaddr *)&servaddr, &servaddr_len);
     if (recv_bytes < 0) {
         perror("recvfrom failed");
         close(sockfd);
         exit(EXIT_FAILURE);
     }
     double end_time = current_time_in_seconds();
 
     recv_buffer[recv_bytes] = '\0';  // Null-terminate the received message
 
     // Calculate the round-trip time and one-way latency estimate
     double rtt = end_time - start_time;
     double latency = rtt / 2;
 
     // Calculate throughput based on one-way latency.
     // Throughput = (number of bytes transmitted) / (one-way delay)
     size_t msg_len = strlen(send_buffer);
     double throughput = (double)msg_len / latency; // in bytes per second
 
     // round-trip throughput:
     double round_trip_throughput = (2.0 * msg_len) / rtt;
 
     printf("Sent message: %s\n", send_buffer);
     printf("Received echo: %s\n", recv_buffer);
     printf("Round-trip time: %.9f seconds\n", rtt);
     printf("Approximate one-way latency: %.9f seconds\n", latency);
     printf("Throughput (one-way): %.9f bytes/second\n", throughput);
     printf("Round-trip throughput: %.9f bytes/second\n", round_trip_throughput);
 
     close(sockfd);
     return 0;
 }
 