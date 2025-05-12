/**
 * This UDP client "spams" the server with test messages,
 * computes the latency and throughput, and records inter-request delays.
 */

#include "udp_test.h"
#include <inttypes.h> // For PRIu64, SCNu64
#include <stdlib.h>   // For malloc, realloc, free, exit
#include <stdio.h>    // For FILE, fopen, fprintf, fclose, perror
#include <string.h>   // For memset, strlen, strerror
#include <errno.h>    // For errno

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>

#define DPDK_PORT_ID 0         // Port ID of the NIC that DPDK uses
#define NUM_MBUFS 8191         // Number of mbufs in mempool. Should be > RX_RING_SIZE + TX_RING_SIZE + some_buffer
#define MBUF_CACHE_SIZE 250    // Cache size for the mempool.
#define RX_RING_SIZE 1024      // Size of RX ring. Power of 2.
#define TX_RING_SIZE 1024      // Size of TX ring. Power of 2.
#define BURST_SIZE 32          // Number of packets to process in a burst.

#define CLIENT_MAC_STR "08:c0:eb:53:ca:64" 
#define CLIENT_IP_ADDR_STR "198.6.245.108"
#define CLIENT_UDP_PORT 9000
#define SERVER_MAC_STR "AA:BB:CC:DD:EE:FF" // PLACEHOLDER

#define IP_DEFTTL 64
#define IP_VERSION 0x40 // ipv4
#define IP_HDRLEN 0x05 // Default IP header length is 5 32-bit words

static struct rte_mempool *mbuf_pool;
static struct rte_ether_addr client_eth_addr;
static struct rte_ether_addr server_eth_addr;
static uint32_t client_ip_addr;
static uint32_t server_ip_addr;

volatile bool force_quit = false;

#define MAX_LATENCY_SAMPLES 1000000 // Max number of latency samples to store
double latency_samples[MAX_LATENCY_SAMPLES];
uint64_t latency_sample_count = 0;

static int
parse_mac_addr(const char *mac_str, struct rte_ether_addr *eth_addr) {
    if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &eth_addr->addr_bytes[0], &eth_addr->addr_bytes[1],
               &eth_addr->addr_bytes[2], &eth_addr->addr_bytes[3],
               &eth_addr->addr_bytes[4], &eth_addr->addr_bytes[5]) != 6) {
        return -1;
    }
    return 0;
}

static int 
port_init(uint16_t port_id, struct rte_mempool *pool) {
    struct rte_eth_conf port_conf_default = {
        .rxmode = {
            .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM, // Enable IP checksum offload on RX
        },
        .txmode = {
            .offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | // Enable IP checksum offload on TX 
                        RTE_ETH_TX_OFFLOAD_UDP_CKSUM, // Enable UDP checksum offload on TX
        }
    };

    const uint16_t rx_rings = 1, tx_rings = 1;
    int err;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port_id)) {
        return -1;
    }

    err = rte_eth_dev_info_get(port_id, &dev_info);
    if (err != 0) {
        printf("Error during getting device (port %u) info: %s\n", port_id, strerror(-err));
        return err;
    }

    struct rte_eth_conf port_conf = port_conf_default;

    err = rte_eth_dev_configure(port_id, rx_rings, tx_rings, &port_conf);
    if (err != 0) {
        printf("rte_eth_dev_configure: err=%d, port=%u\n", err, port_id);
        return err;
    }

    // Allocate and set up "rx_rings" RX queues per Ethernet port.
    for (q = 0; q < rx_rings; q++) {
        err = rte_eth_rx_queue_setup(port_id, q, RX_RING_SIZE, rte_eth_dev_socket_id(port_id), NULL, pool);
        if (err < 0) {
            printf("rte_eth_rx_queue_setup: err=%d, port=%u\n", err, port_id);
            return err;
        }
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    // Allocate and set up "tx_rings" TX queues per Ethernet port.
    for (q = 0; q < tx_rings; q++) {
        err = rte_eth_tx_queue_setup(port_id, q, TX_RING_SIZE, rte_eth_dev_socket_id(port_id), &txconf);
        if (err < 0) {
            printf("rte_eth_tx_queue_setup: err=%d, port=%u\n", err, port_id);
            return err;
        }
    }

    err = rte_eth_dev_start(port_id);
    if (err < 0) {
        printf("rte_eth_dev_start: err=%d, port=%u\n", err, port_id);
        return err;
    }

    err = rte_eth_macaddr_get(port_id, &client_eth_addr);
    if (err != 0) {
         printf("rte_eth_macaddr_get: err=%d, port=%u\n", err, port_id);
        return err;
    }
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           port_id, RTE_ETHER_ADDR_BYTES(&client_eth_addr));
    return 0;

}

int main(int argc, char *argv[]) {
    int ret; 
    int sockfd;
    struct sockaddr_in servaddr;
    char send_buffer[MAX_MSG_SIZE];
    FILE *delay_file = NULL; // File pointer for delays
    const char* delay_filename = "delays.txt";

    double *timestamps = NULL;       // Pointer to hold timestamps
    size_t timestamp_count = 0;      // Number of timestamps recorded
    size_t timestamp_capacity = 0;   // Allocated capacity of the array

    // Initialize the Environment Abstraction Layer (EAL)
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_panic("ERROR: Cannot init EAL\n");
    }

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

    // Allocate initial memory for timestamps
    timestamp_capacity = MAX_LATENCY_SAMPLES;
    timestamps = (double *)malloc(timestamp_capacity * sizeof(double));
    if (timestamps == NULL) {
        perror("Failed to allocate initial memory for timestamps");
        close(sockfd);
        exit(EXIT_FAILURE);
    }


    // Begin latency measurement
    double start_time = current_time_in_seconds();
    unsigned long sent_messages = 0;
    unsigned long total_bytes_sent = 0;
    unsigned long iterations = 0;


    while (current_time_in_seconds() - start_time < TEST_DURATION) {
        iterations++;

        double before_send_time = current_time_in_seconds(); // Record time before sending

        ssize_t ret = sendto(sockfd, send_buffer, MAX_MSG_SIZE, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // If sendto would block, don't record this timestamp or count it as sent
                continue;
            } else {
                perror("sendto failure");
                break; // Exit loop on other errors
            }
        } else {
            // Ignore if we exceed array size - we already have enough samples. 
            if (timestamp_count >= timestamp_capacity) {
                continue;
            }

            // Store timestamp only if send succeeded and array has space
            if (timestamp_count < timestamp_capacity) {
                timestamps[timestamp_count++] = before_send_time;
            }

            sent_messages++;
            total_bytes_sent += ret;
        }
    }
    double end_time = current_time_in_seconds();
    double elapsed_time = end_time - start_time;

    // Send termination message
    for (int i = 0; i < 3; i++) {
        sendto(sockfd, TERMINATE_MSG, strlen(TERMINATE_MSG), 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
        // Small sleep to increase chances of termination message getting through
        struct timespec req = {0, 100000000}; // 100 ms
        nanosleep(&req, NULL);
    }

    // Calculate and print statistics
    double avg_latency = (sent_messages > 0) ? (elapsed_time / (2.0 * sent_messages)) : 0.0;
    double throughput = (elapsed_time > 0) ? (total_bytes_sent / elapsed_time) : 0.0;
    printf("Sent %lu messages out of %lu attempts, %lu bytes in %.2f seconds\n",
        sent_messages, iterations, total_bytes_sent, elapsed_time);
    printf("Recorded %zu timestamps for delay calculation.\n", timestamp_count);
    printf("Approximate average one-way latency: %.9f seconds\n", avg_latency);
    printf("Throughput: %.9f Mbps\n", throughput * (8.0 / 1e6));

    // Calculate delays and write to file
    if (timestamp_count > 1) {
        delay_file = fopen(delay_filename, "w");
        if (delay_file == NULL) {
            perror("Failed to open delay file for writing");
            // Continue without writing delays if file cannot be opened
        } else {
            printf("Calculating and writing delays to %s...\n", delay_filename);
            fprintf(delay_file, "Delay_Seconds\n"); // Write header
            for (size_t i = 1; i < timestamp_count; ++i) {
                double delay = timestamps[i] - timestamps[i - 1];
                // Write delay with high precision, check for write errors
                if (fprintf(delay_file, "%.9f\n", delay) < 0) {
                    perror("Error writing delay to file");
                    // Optionally break loop or just report error
                    break;
                }
            }
            if (fclose(delay_file) != 0) { // Check fclose return value
                perror("Error closing delay file");
            } else {
                printf("Finished writing delays.\n");
            }
        }
    } else {
        printf("Not enough timestamps recorded (%zu) to calculate delays.\n", timestamp_count);
    }
    // Free the allocated memory for timestamps
    free(timestamps);
    timestamps = NULL; // Good practice to NULL pointer after freeing


    // Set up TCP connection to receive server statistics
    int tcp_sockfd = -1; // Initialize to invalid state
    struct sockaddr_in tcp_servaddr;

    tcp_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sockfd < 0) {
        perror("TCP socket creation failed");
        // No exit here, just proceed without server stats if TCP fails
    } else {
        memset(&tcp_servaddr, 0, sizeof(tcp_servaddr));
        tcp_servaddr.sin_family = AF_INET;
        tcp_servaddr.sin_port = htons(TCP_PORT);
        // Use inet_pton for safety
        if (inet_pton(AF_INET, REMOTE_ADDRESS, &tcp_servaddr.sin_addr) <= 0) {
            perror("Invalid address or address not supported for TCP");
            close(tcp_sockfd); // Close TCP socket if address is invalid
            tcp_sockfd = -1; // Mark as invalid
        } else {
        // Add a delay before attempting to connect
        sleep(2); // Increased sleep slightly

        if (connect(tcp_sockfd, (struct sockaddr*)&tcp_servaddr, sizeof(tcp_servaddr)) < 0) {
            perror("TCP connection failed");
            close(tcp_sockfd);
            tcp_sockfd = -1; // Mark as invalid
        }
        }
    }


    // Receive server statistics only if TCP setup was successful
    if (tcp_sockfd != -1) {
        char stats_buffer[256];
        ssize_t bytes_received = recv(tcp_sockfd, stats_buffer, sizeof(stats_buffer) - 1, 0);
        if (bytes_received < 0) {
            perror("Failed to receive server statistics");
        } else if (bytes_received == 0) {
            printf("Server closed TCP connection before sending stats.\n");
        } else {
            stats_buffer[bytes_received] = '\0'; // Null-terminate the received string
            uint64_t server_messages_received = 0; // Initialize
            uint64_t server_total_bytes_received = 0; // Initialize
            double server_elapsed_time = 0.0; // Initialize

            // Use sscanf carefully, check return value
            int items_scanned = sscanf(stats_buffer, "%" SCNu64 " %" SCNu64 " %lf",
                                    &server_messages_received,
                                    &server_total_bytes_received,
                                    &server_elapsed_time);

            if (items_scanned == 3) {
                // Avoid division by zero
                double server_latency = (server_messages_received > 0) ? (server_elapsed_time / (2.0 * server_messages_received)) : 0.0;
                double server_throughput = (server_elapsed_time > 0) ? (server_total_bytes_received / server_elapsed_time) : 0.0;

                printf("\nServer Statistics:\n");
                printf("Received %" PRIu64 " messages, %" PRIu64 " bytes in %.2f seconds (reported by server)\n",
                        server_messages_received, server_total_bytes_received, server_elapsed_time);
                // Note: Server latency/throughput calculated here might differ slightly
                // from server's own calculation depending on exact timing measurements.
                printf("Approximate server one-way latency: %.9f seconds\n", server_latency);
                printf("Server throughput: %.9f Mbps\n", server_throughput * (8.0 / 1e6));
            } else {
                printf("Failed to parse server statistics string (expected 3 items, got %d): '%s'\n", items_scanned, stats_buffer);
            }
        }
        close(tcp_sockfd); // Close TCP socket after use
    }
    // Close UDP socket
    close(sockfd);

    // Clean up the EAL
    // This should be called when the application is exiting.
    rte_eal_cleanup();
    printf("EAL cleaned up.\n");
    return 0;
}
