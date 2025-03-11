/**
 * This UDP server listens on a fixed port (12345) and echoes back any message it receives.
 */
#include "udp_test.h";

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

    unsigned long messages_received = 0;
    unsigned long total_bytes_received = 0;
    // Main loop: receive datagrams and acknowledge the client
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
        messages_received++;
        total_bytes_received += n;
    }

    close(sockfd);
    return 0;
}
