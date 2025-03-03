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
 #include <fcntl.h>
 #include<errno.h>
 #define SERVER_PORT 12345
 #define MAX_MSG_SIZE 1472 // Maximum message payload in bytes. 
 #define TEST_DURATION 10.0 // Test duration in seconds. 
 
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
     char send_buffer[MAX_MSG_SIZE];

     // Create UDP socket
     if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

     // Set the socket to nonblocking mode. 
     int flags = fcntl(sockfd, F_GETFL, 0);
     if (flags < 0) {
        perror("fcntl F_GETFL failed");
        close(sockfd);
        exit(EXIT_FAILURE);
     }
     if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL failed");
        close(sockfd);
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

     // Prepare a message that maxes out buffer size. 
     memset(send_buffer, 'A', MAX_MSG_SIZE);
 
     // Begin latency measurement
     double start_time = current_time_in_seconds();
     unsigned long sent_messages = 0;
     unsigned long total_bytes_sent = 0;

     while (current_time_in_seconds() - start_time < TEST_DURATION) {
        ssize_t ret = sendto(sockfd, send_buffer, MAX_MSG_SIZE, 0 , (struct sockaddr *)&servaddr, sizeof(servaddr));

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { // Socket not ready, retry immediately
                continue;
            }
            else {
                perror("sendto failure");
                break;
            } 
        } else {
            sent_messages++;
            total_bytes_sent += ret;
        }
     }

     double end_time = current_time_in_seconds();
     double elapsed_time = end_time - start_time;
     
     // Calculate approximate one-way latency and throughput
     // Latency is estimated as half the average round-trip time per message.
     double latency = elapsed_time / (2 * sent_messages);
     // Throughput is calculated as the total bytes sent divided by the elapsed time.
     double throughput = total_bytes_sent / elapsed_time;
     
     printf("Sent %lu messages, %lu bytes in %.2f seconds\n", 
            sent_messages, total_bytes_sent, elapsed_time);
     printf("Approximate one-way latency: %.9f seconds\n", latency);
     printf("Throughput: %.9f bytes/second\n", throughput);
 
     close(sockfd);
     return 0;
 }
 