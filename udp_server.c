/**
 * This UDP server listens on a fixed port (12345) and echoes back any message it receives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SERVER_PORT 12345
#define BUF_SIZE 1024

int main(void);

int main(void) {
    int sockfd;
    char buffer[BUF_SIZE];
    struct sockaddr_in servaddr, cliaddr;
    socklen_t cliaddr_len = sizeof(cliaddr);

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Zero out the server and client address structures
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // Set up server address structure
    servaddr.sin_family = AF_INET;          // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;    // Listen on all interfaces
    servaddr.sin_port = htons(SERVER_PORT);   // Set port number

    // Bind the socket to the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("UDP server listening on port %d...\n", SERVER_PORT);

    // Main loop: receive datagrams and echo them back to the sender
    while (1) {
        ssize_t n = recvfrom(sockfd, (char *)buffer, BUF_SIZE, 0,
                             (struct sockaddr *)&cliaddr, &cliaddr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        buffer[n] = '\0';  // Null-terminate the received data
        printf("Received from %s:%d: %s\n",
               inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port), buffer);

        // Echo the message back to the sender
        if (sendto(sockfd, (const char *)buffer, n, 0,
                   (const struct sockaddr *)&cliaddr, cliaddr_len) < 0) {
            perror("sendto failed");
        }
    }

    close(sockfd);
    return 0;
}
