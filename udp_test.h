/**
 * Common definitions for UDP client-server throughput/latency test
 */
#ifndef UDP_COMMON_H
#define UDP_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/time.h>


// Common constants
#define SERVER_PORT 12345
#define MAX_MSG_SIZE 1472
#define TEST_DURATION 5.0
#define REMOTE_ADDRESS "127.0.0.1"
#define TCP_PORT 12346  // Separate port for TCP statistics

// Statistics structure
typedef struct {
    uint64_t messages_received;
    uint64_t total_bytes_received;
    double avg_message_size;
} ServerStats;

// Function prototypes
double current_time_in_seconds(void);
int set_socket_nonblocking(int sockfd);

// Get current time in seconds with high precision
double current_time_in_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Set socket to non-blocking mode
int set_socket_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL failed");
        return -1;
    }
    
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL failed");
        return -1;
    }
    
    return 0;
}

const char* TERMINATE_MSG = "TERMINATE_TEST";

#endif // UDP_COMMON_H
