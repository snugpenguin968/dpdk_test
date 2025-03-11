/**
 * This UDP server listens on a fixed port (12345) and echoes back any message it receives.
 */
#include "udp_test.h"

#define TERMINATE_MSG "TERMINATE_TEST"
#define TCP_PORT 12346

int main(void);

int main(void) {
    int sockfd;
    char buffer[MAX_MSG_SIZE];
    struct sockaddr_in servaddr, cliaddr;
    socklen_t cliaddr_len = sizeof(cliaddr);

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

    // Zero out the server and client address structures
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // Set up server address structure
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    servaddr.sin_port = htons(SERVER_PORT); // Set port number

    // Bind the socket to the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("UDP server listening on port %d...\n", SERVER_PORT);

    ServerStats stats = {0};
    char client_ip[INET_ADDRSTRLEN];

    // Main loop: receive datagrams and track statistics.
    while (1) {
        ssize_t n = recvfrom(sockfd, (char *)buffer, MAX_MSG_SIZE, 0,
                             (struct sockaddr *)&cliaddr, &cliaddr_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available, continue loop
                continue;
            } else {
                perror("recvfrom failed");
                continue;
            }
        }
        // Ensure null terminator for string comparison
        buffer[n < MAX_MSG_SIZE ? n : MAX_MSG_SIZE-1] = '\0';

        // Store client IP
        inet_ntop(AF_INET, &cliaddr.sin_addr, client_ip, INET_ADDRSTRLEN);

        if (strcmp(buffer, TERMINATE_MSG) == 0) {
            printf("Received termination message from %s. Setting up TCP connection...\n", client_ip);
            break; // Break loop to send statistics
        } else {
            stats.messages_received++;
            stats.total_bytes_received += n;
        }
    }

    if (stats.messages_received > 0) {
        stats.avg_message_size = (double)stats.total_bytes_received / stats.messages_received;
    }
    close(sockfd);

    int tcp_sockfd, new_socket;
    struct sockaddr_in tcp_servaddr, tcp_cliaddr;
    socklen_t tcp_cliaddr_len = sizeof(tcp_cliaddr);

    // Create tcp socket
    if ((tcp_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set up TCP server address
    memset(&tcp_servaddr, 0, sizeof(tcp_servaddr));
    tcp_servaddr.sin_family = AF_INET;
    tcp_servaddr.sin_addr.s_addr = INADDR_ANY;
    tcp_servaddr.sin_port = htons(TCP_PORT);

    // Bind TCP socket
    if (bind(tcp_sockfd, (struct sockaddr*)&tcp_servaddr, sizeof(tcp_servaddr)) < 0) {
        perror("TCP bind failed");
        close(tcp_sockfd);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(tcp_sockfd, 1) < 0) {
        perror("TCP listen failed");
        close(tcp_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("TCP server waiting for connection on port %d...\n", TCP_PORT);

    // Accept connection from client
    if ((new_socket = accept(tcp_sockfd, (struct sockaddr*)&tcp_cliaddr, &tcp_cliaddr_len)) < 0) {
        perror("TCP accept failed");
        close(tcp_sockfd);
        exit(EXIT_FAILURE);
    }
    
    // Send statistics structure to client
    if (send(new_socket, &stats, sizeof(stats), 0) < 0) {
        perror("Failed to send statistics");
    } else {
        printf("Statistics sent to client via TCP\n");
    }
    
    // Close TCP connections
    close(new_socket);
    close(tcp_sockfd);

    return 0;
}
