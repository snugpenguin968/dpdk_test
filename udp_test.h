/**
 * Common definitions for UDP client-server throughput/latency test
 */
#ifndef UDP_COMMON_H
#define UDP_COMMON_H

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

// Common constants
#define SERVER_PORT 12345
#define MAX_MSG_SIZE 1472
#define TEST_DURATION 10.0
#define REMOTE_ADDRESS "168.6.245.109"

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

#endif