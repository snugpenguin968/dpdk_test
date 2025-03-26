#include "udp_test.h"
#include <inttypes.h>

int main(void) {
    int sockfd;
    struct sockaddr_in servaddr;
    char send_buffer[MAX_MSG_SIZE];

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set the socket to nonblocking mode
    if (set_socket_nonblocking(sockfd) < 0) {
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Set up server address structure
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, REMOTE_ADDRESS, &servaddr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Prepare a message that maxes out buffer size
    memset(send_buffer, 'A', MAX_MSG_SIZE);

    // Begin latency measurement
    double start_time = current_time_in_seconds();
    unsigned long sent_messages = 0;
    unsigned long total_bytes_sent = 0;
    unsigned long iterations = 0;

    while (current_time_in_seconds() - start_time < TEST_DURATION) {
        iterations++;
        ssize_t ret = sendto(sockfd, send_buffer, MAX_MSG_SIZE, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
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

    // Send termination message
    for (int i = 0; i < 3; i++) {
        sendto(sockfd, TERMINATE_MSG, strlen(TERMINATE_MSG), 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
        sleep(1); 
    }

    // Calculate and print statistics
    double latency = elapsed_time / (2 * sent_messages);
    double throughput = total_bytes_sent / elapsed_time;
    printf("Sent %lu messages out of %lu attempts, %lu bytes in %.2f seconds\n",
           sent_messages, iterations, total_bytes_sent, elapsed_time);
    printf("Approximate one-way latency: %.9f seconds\n", latency);
    printf("Throughput: %.9f Mbps\n", throughput * (8 / 1e6));

    // Set up TCP connection to receive server statistics
    int tcp_sockfd;
    struct sockaddr_in tcp_servaddr;

    if ((tcp_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&tcp_servaddr, 0, sizeof(tcp_servaddr));
    tcp_servaddr.sin_family = AF_INET;
    tcp_servaddr.sin_port = htons(TCP_PORT);
    tcp_servaddr.sin_addr.s_addr = inet_addr(REMOTE_ADDRESS);

    // Add a delay before attempting to connect
    sleep(1);

    if (connect(tcp_sockfd, (struct sockaddr*)&tcp_servaddr, sizeof(tcp_servaddr)) < 0) {
        perror("TCP connection failed");
        close(tcp_sockfd);
        exit(EXIT_FAILURE);
    }

    // Receive server statistics
    char stats_buffer[256];
    ssize_t bytes_received = recv(tcp_sockfd, stats_buffer, sizeof(stats_buffer) - 1, 0);
    if (bytes_received < 0) {
        perror("Failed to receive server statistics");
    } else {
        stats_buffer[bytes_received] = '\0';
        uint64_t server_messages_received, server_total_bytes_received;
        double server_elapsed_time;

        sscanf(stats_buffer, "%" PRIu64 " %" PRIu64 " %lf",
            &server_messages_received,
            &server_total_bytes_received,
            &server_elapsed_time);

        double server_latency = server_elapsed_time / (2 * server_messages_received);
        double server_throughput = server_total_bytes_received / server_elapsed_time;

        printf("\nServer Statistics:\n");
        printf("Received %"PRIu64" messages, %"PRIu64" bytes in %.2f seconds\n",
            server_messages_received, server_total_bytes_received, server_elapsed_time);
        printf("Approximate one-way latency: %.9f seconds\n", server_latency);
        printf("Throughput: %.9f Mbps\n", server_throughput * (8 / 1e6));
    }


    // Close sockets
    close(sockfd);
    close(tcp_sockfd);

    return 0;
}
