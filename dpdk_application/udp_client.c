/**
 * This UDP client "spams" the server with test messages,
 * computes the latency and throughput, and records inter-request delays.
 * It then fetches statistics from the server via TCP.
 */

#include "udp_test.h"
#include <arpa/inet.h>  // For inet_pton
#include <errno.h>      // For errno
#include <inttypes.h>   // For PRIu64, SCNu64
#include <netinet/in.h> // For sockaddr_in
#include <signal.h>
#include <stdio.h>      // For FILE, fopen, fprintf, fclose, perror
#include <stdlib.h>     // For malloc, realloc, free, exit
#include <string.h>     // For memset, strlen, strerror
#include <sys/socket.h> // For TCP sockets
#include <unistd.h>     // For close

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define DPDK_PORT_ID 0 // Port ID of the NIC that DPDK uses
#define NUM_MBUFS                                                              \
  8191 // Number of mbufs in mempool. Should be > RX_RING_SIZE + TX_RING_SIZE +
       // some_buffer
#define MBUF_CACHE_SIZE 250 // Cache size for the mempool.
#define RX_RING_SIZE 1024   // Size of RX ring. Power of 2.
#define TX_RING_SIZE 1024   // Size of TX ring. Power of 2.
#define BURST_SIZE 32       // Number of packets to process in a burst.

#define CLIENT_IP_ADDR_STR "198.6.245.108" // Example client IP
#define CLIENT_UDP_PORT 9000 // Client's source UDP port for the test
#define SERVER_MAC_STR "AA:BB:CC:DD:EE:FF" // *** PLACEHOLDER ***

#define IP_DEFTTL 64
#define IP_VERSION 0x40 // ipv4
#define IP_HDRLEN 0x05  // Default IP header length is 5 32-bit words (20 bytes)

static struct rte_mempool *mbuf_pool;
static struct rte_ether_addr
    client_eth_addr; // Will be read from NIC by port_init
static struct rte_ether_addr server_eth_addr; // Parsed from SERVER_MAC_STR
static uint32_t client_ip_addr;               // Parsed from CLIENT_IP_ADDR_STR
static uint32_t server_ip_addr; // Parsed from REMOTE_ADDRESS (udp_test.h)

volatile bool force_quit = false;

#define MAX_LATENCY_SAMPLES 1000000 // Max number of latency samples to store
double latency_samples[MAX_LATENCY_SAMPLES];
uint64_t latency_sample_count = 0; // Global counter for latency_samples

// Function to parse MAC address string "XX:XX:XX:XX:XX:XX"
static int parse_mac_addr(const char *mac_str,
                          struct rte_ether_addr *eth_addr) {
  if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &eth_addr->addr_bytes[0],
             &eth_addr->addr_bytes[1], &eth_addr->addr_bytes[2],
             &eth_addr->addr_bytes[3], &eth_addr->addr_bytes[4],
             &eth_addr->addr_bytes[5]) != 6) {
    return -1;
  }
  return 0;
}

// Function to parse IP address string "A.B.C.D" to uint32_t in network byte
// order
static int parse_ip_addr(const char *ip_str, uint32_t *ip_addr) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_str, &addr) == 1) {
    *ip_addr = addr.s_addr; // Already in network byte order
    return 0;
  }
  return -1;
}

static int port_init(uint16_t port_id, struct rte_mempool *pool) {
  struct rte_eth_conf port_conf_default = {
      .rxmode =
          {
              .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
          },
      .txmode = {
          .offloads =
              RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
      }};

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
    printf("Error during getting device (port %u) info: %s\n", port_id,
           strerror(-err));
    return err;
  }

  struct rte_eth_conf port_conf = port_conf_default;

  err = rte_eth_dev_configure(port_id, rx_rings, tx_rings, &port_conf);
  if (err != 0) {
    printf("rte_eth_dev_configure: err=%d, port=%u\n", err, port_id);
    return err;
  }

  for (q = 0; q < rx_rings; q++) {
    err = rte_eth_rx_queue_setup(port_id, q, RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), NULL, pool);
    if (err < 0) {
      printf("rte_eth_rx_queue_setup: err=%d, port=%u\n", err, port_id);
      return err;
    }
  }

  txconf = dev_info.default_txconf;
  txconf.offloads = port_conf.txmode.offloads;

  for (q = 0; q < tx_rings; q++) {
    err = rte_eth_tx_queue_setup(port_id, q, TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), &txconf);
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
  printf("Port %u MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
         ":%02" PRIx8 ":%02" PRIx8 "\n",
         port_id, client_eth_addr.addr_bytes[0], client_eth_addr.addr_bytes[1],
         client_eth_addr.addr_bytes[2], client_eth_addr.addr_bytes[3],
         client_eth_addr.addr_bytes[4], client_eth_addr.addr_bytes[5]);
  return 0;
}

// For constructing packets with timestamp in payload
static void construct_udp_packet_with_timestamp(struct rte_mbuf *mbuf,
                                                uint16_t payload_size,
                                                uint64_t timestamp_seq) {
  struct rte_ether_hdr *eth_h;
  struct rte_ipv4_hdr *ip_h;
  struct rte_udp_hdr *udp_h;
  char *payload;
  uint16_t pkt_len;

  mbuf->pkt_len = 0;
  mbuf->data_len = 0;

  eth_h = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
  rte_ether_addr_copy(&client_eth_addr, &eth_h->src_addr);
  rte_ether_addr_copy(&server_eth_addr, &eth_h->dst_addr);
  eth_h->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  pkt_len = sizeof(struct rte_ether_hdr);

  ip_h = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(mbuf, char *) + pkt_len);
  ip_h->version_ihl = IP_VERSION | IP_HDRLEN;
  ip_h->type_of_service = 0;
  ip_h->total_length = rte_cpu_to_be_16(
      sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_size);
  ip_h->packet_id = 0;
  ip_h->fragment_offset = 0;
  ip_h->time_to_live = IP_DEFTTL;
  ip_h->next_proto_id = IPPROTO_UDP;
  ip_h->hdr_checksum = 0;
  ip_h->src_addr = client_ip_addr;
  ip_h->dst_addr = server_ip_addr;
  pkt_len += sizeof(struct rte_ipv4_hdr);

  udp_h = (struct rte_udp_hdr *)(rte_pktmbuf_mtod(mbuf, char *) + pkt_len);
  udp_h->src_port = rte_cpu_to_be_16(CLIENT_UDP_PORT);
  udp_h->dst_port =
      rte_cpu_to_be_16(SERVER_PORT); // SERVER_PORT from udp_test.h
  udp_h->dgram_len =
      rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_size);
  udp_h->dgram_cksum = 0;
  pkt_len += sizeof(struct rte_udp_hdr);

  payload = rte_pktmbuf_mtod_offset(mbuf, char *, pkt_len);
  *(uint64_t *)payload = rte_cpu_to_be_64(timestamp_seq);
  for (uint16_t i = sizeof(uint64_t); i < payload_size; i++) {
    payload[i] = (char)('A' + (i % 26));
  }
  pkt_len += payload_size;

  mbuf->data_len = pkt_len;
  mbuf->pkt_len = pkt_len;
  mbuf->l2_len = sizeof(struct rte_ether_hdr);
  mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
  mbuf->ol_flags =
      RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
}

// For constructing packets with a custom string payload (e.g., TERMINATE_MSG)
static void construct_udp_packet_with_raw_payload(struct rte_mbuf *mbuf,
                                                  const char *raw_payload_data,
                                                  uint16_t raw_payload_size) {
  struct rte_ether_hdr *eth_h;
  struct rte_ipv4_hdr *ip_h;
  struct rte_udp_hdr *udp_h;
  char *payload_ptr;
  uint16_t pkt_len;

  mbuf->pkt_len = 0;
  mbuf->data_len = 0;

  eth_h = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
  rte_ether_addr_copy(&client_eth_addr, &eth_h->src_addr);
  rte_ether_addr_copy(&server_eth_addr, &eth_h->dst_addr);
  eth_h->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  pkt_len = sizeof(struct rte_ether_hdr);

  ip_h = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(mbuf, char *) + pkt_len);
  ip_h->version_ihl = IP_VERSION | IP_HDRLEN;
  ip_h->type_of_service = 0;
  // total_length set after payload
  ip_h->packet_id = rte_cpu_to_be_16(0);
  ip_h->fragment_offset = rte_cpu_to_be_16(0);
  ip_h->time_to_live = IP_DEFTTL;
  ip_h->next_proto_id = IPPROTO_UDP;
  ip_h->hdr_checksum = 0;
  ip_h->src_addr = client_ip_addr;
  ip_h->dst_addr = server_ip_addr;
  pkt_len += sizeof(struct rte_ipv4_hdr);

  udp_h = (struct rte_udp_hdr *)(rte_pktmbuf_mtod(mbuf, char *) + pkt_len);
  udp_h->src_port = rte_cpu_to_be_16(CLIENT_UDP_PORT);
  udp_h->dst_port = rte_cpu_to_be_16(SERVER_PORT);
  // dgram_len set after payload
  udp_h->dgram_cksum = 0;
  pkt_len += sizeof(struct rte_udp_hdr);

  payload_ptr = rte_pktmbuf_mtod_offset(mbuf, char *, pkt_len);
  rte_memcpy(payload_ptr, raw_payload_data, raw_payload_size);
  pkt_len += raw_payload_size;

  ip_h->total_length =
      rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
                       sizeof(struct rte_udp_hdr) + raw_payload_size);
  udp_h->dgram_len =
      rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + raw_payload_size);

  mbuf->data_len = pkt_len;
  mbuf->pkt_len = pkt_len;
  mbuf->l2_len = sizeof(struct rte_ether_hdr);
  mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
  mbuf->ol_flags =
      RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
}

static void signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    printf("\n\nSignal %d received, preparing to exit...\n", signum);
    force_quit = true;
  }
}

int main(int argc, char *argv[]) {
  int ret;
  unsigned lcore_id;
  uint16_t port_id = DPDK_PORT_ID;
  uint64_t total_packets_tx = 0;
  uint64_t total_packets_rx = 0;
  uint64_t total_bytes_tx = 0;
  uint64_t total_bytes_rx = 0;
  uint64_t tsc_hz = rte_get_tsc_hz();
  double test_start_time, test_end_time, test_duration_actual;
  uint64_t current_send_timestamp_seq = 0;

  ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
  argc -= ret;
  argv += ret;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  if (rte_eth_dev_count_avail() == 0)
    rte_exit(EXIT_FAILURE, "No ethernet ports available\n");

  if (!rte_eth_dev_is_valid_port(port_id))
    rte_exit(EXIT_FAILURE, "Port %u is invalid\n", port_id);

  if (parse_mac_addr(SERVER_MAC_STR, &server_eth_addr) != 0) {
    rte_exit(EXIT_FAILURE,
             "Invalid SERVER_MAC_STR format: %s. Please set the correct MAC.\n",
             SERVER_MAC_STR);
  }
  if (parse_ip_addr(CLIENT_IP_ADDR_STR, &client_ip_addr) != 0) {
    rte_exit(EXIT_FAILURE, "Invalid CLIENT_IP_ADDR_STR format: %s\n",
             CLIENT_IP_ADDR_STR);
  }
  if (parse_ip_addr(REMOTE_ADDRESS, &server_ip_addr) != 0) {
    rte_exit(EXIT_FAILURE,
             "Invalid SERVER_IP_ADDR_STR (REMOTE_ADDRESS) format: %s\n",
             REMOTE_ADDRESS);
  }

  unsigned num_lcores = rte_lcore_count();
  mbuf_pool = rte_pktmbuf_pool_create(
      "MBUF_POOL_CLIENT", NUM_MBUFS * num_lcores, MBUF_CACHE_SIZE, 0,
      RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (mbuf_pool == NULL)
    rte_exit(EXIT_FAILURE, "Cannot create mbuf pool: %s\n",
             rte_strerror(rte_errno));

  if (port_init(port_id, mbuf_pool) != 0)
    rte_exit(EXIT_FAILURE, "Cannot init port %u\n", port_id);

  printf(
      "Client using Source IP: %s, Dest IP: %s, Dest Port (UDP for test): %d\n",
      CLIENT_IP_ADDR_STR, REMOTE_ADDRESS, SERVER_PORT);
  printf("Client will send to Dest MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
         ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 "\n",
         server_eth_addr.addr_bytes[0], server_eth_addr.addr_bytes[1],
         server_eth_addr.addr_bytes[2], server_eth_addr.addr_bytes[3],
         server_eth_addr.addr_bytes[4], server_eth_addr.addr_bytes[5]);

  lcore_id = rte_lcore_id();
  printf("Client starting on lcore %u (main lcore)\n", lcore_id);

  printf("Waiting for link to come up on port %u...\n", port_id);
  struct rte_eth_link link;
  int link_wait_count = 0;
  do {
    rte_eth_link_get_nowait(port_id, &link);
    if (link.link_status)
      break;
    rte_delay_ms(100);
    link_wait_count++;
  } while (link_wait_count < 90 && !force_quit);

  if (!link.link_status) {
    printf("Link down on port %u after waiting. Exiting.\n", port_id);
    rte_eal_cleanup();
    return -1;
  }
  printf("Link up on port %u (Speed %u Mbps, %s)\n", port_id, link.link_speed,
         (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "full-duplex"
                                                        : "half-duplex");

  printf("Starting UDP test for %.2f seconds...\n", TEST_DURATION);
  test_start_time = current_time_in_seconds();
  test_end_time = test_start_time + TEST_DURATION;

  struct rte_mbuf *tx_mbufs[BURST_SIZE];
  struct rte_mbuf *rx_mbufs[BURST_SIZE];
  uint16_t nb_tx, nb_rx;
  uint64_t last_stat_display_time = rte_rdtsc();

  for (int i = 0; i < BURST_SIZE; i++) {
    tx_mbufs[i] = rte_pktmbuf_alloc(mbuf_pool);
    if (tx_mbufs[i] == NULL) {
      rte_exit(EXIT_FAILURE, "Failed to allocate tx mbuf %d\n", i);
    }
  }

  while (current_time_in_seconds() < test_end_time && !force_quit) {
    for (int i = 0; i < BURST_SIZE; i++) {
      current_send_timestamp_seq = rte_rdtsc();
      construct_udp_packet_with_timestamp(tx_mbufs[i], MAX_MSG_SIZE,
                                          current_send_timestamp_seq);
    }

    nb_tx = rte_eth_tx_burst(port_id, 0, tx_mbufs, BURST_SIZE);
    total_packets_tx += nb_tx;
    for (int i = 0; i < nb_tx; i++) {
      total_bytes_tx += tx_mbufs[i]->pkt_len;
    }


    nb_rx = rte_eth_rx_burst(port_id, 0, rx_mbufs, BURST_SIZE);
    if (nb_rx > 0) {
      total_packets_rx += nb_rx;
      for (int i = 0; i < nb_rx; i++) {
        struct rte_mbuf *m = rx_mbufs[i];
        total_bytes_rx += m->pkt_len;
        struct rte_ether_hdr *eth_h =
            rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        if (eth_h->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
          struct rte_ipv4_hdr *ip_h = (struct rte_ipv4_hdr *)(eth_h + 1);
          if (ip_h->next_proto_id == IPPROTO_UDP) {
            uint16_t ip_hdr_len_bytes = (ip_h->version_ihl & 0x0F) * 4;
            struct rte_udp_hdr *udp_h =
                (struct rte_udp_hdr *)((char *)ip_h + ip_hdr_len_bytes);
            if (udp_h->dst_port == rte_cpu_to_be_16(CLIENT_UDP_PORT)) {
              char *payload = (char *)(udp_h + 1);
              uint64_t original_send_timestamp_seq =
                  rte_be_to_cpu_64(*(uint64_t *)payload);
              uint64_t rtt_cycles = rte_rdtsc() - original_send_timestamp_seq;

              if (latency_sample_count < MAX_LATENCY_SAMPLES) {
                latency_samples[latency_sample_count++] =
                    (double)rtt_cycles * 1000000.0 / tsc_hz;
              }
            }
          }
        }
        rte_pktmbuf_free(m);
      }
    }
    uint64_t now = rte_rdtsc();
    if ((now - last_stat_display_time) > tsc_hz) {
      double current_test_time_sec = current_time_in_seconds();
      double elapsed = current_test_time_sec - test_start_time;
      if (elapsed > 0) {
        printf("\rTX PPS: %8.0f | RX PPS: %8.0f | TX Mbps: %8.2f | RX Mbps: "
               "%8.2f | Latency Samples: %" PRIu64 " ",
               (double)total_packets_tx / elapsed,
               (double)total_packets_rx / elapsed,
               (double)total_bytes_tx * 8 / (elapsed * 1000000.0),
               (double)total_bytes_rx * 8 / (elapsed * 1000000.0),
               latency_sample_count);
        fflush(stdout);
      }
      last_stat_display_time = now;
    }
  }

  for (int i = 0; i < BURST_SIZE; ++i) {
    rte_pktmbuf_free(tx_mbufs[i]);
  }

  test_duration_actual = current_time_in_seconds() - test_start_time;
  printf("\n\nUDP Test finished in %.2f seconds.\n", test_duration_actual);

  printf("--- Local Client Statistics ---\n");
  printf("Total Packets Sent: %" PRIu64 "\n", total_packets_tx);
  printf("Total Packets Received: %" PRIu64 "\n", total_packets_rx);
  printf("Total Bytes Sent: %" PRIu64 "\n", total_bytes_tx);
  printf("Total Bytes Received: %" PRIu64 "\n", total_bytes_rx);

  if (test_duration_actual > 0) {
    printf("Average TX Throughput: %.2f Mbps\n",
           (double)total_bytes_tx * 8 / (test_duration_actual * 1000000.0));
    printf("Average RX Throughput: %.2f Mbps\n",
           (double)total_bytes_rx * 8 / (test_duration_actual * 1000000.0));
    printf("Average TX Packet Rate: %.0f PPS\n",
           (double)total_packets_tx / test_duration_actual);
    printf("Average RX Packet Rate: %.0f PPS\n",
           (double)total_packets_rx / test_duration_actual);
  }

  if (latency_sample_count > 0) {
    double sum_latency = 0;
    double min_latency = latency_samples[0];
    double max_latency = latency_samples[0];
    for (uint64_t i = 0; i < latency_sample_count; ++i) {
      sum_latency += latency_samples[i];
      if (latency_samples[i] < min_latency)
        min_latency = latency_samples[i];
      if (latency_samples[i] > max_latency)
        max_latency = latency_samples[i];
    }
    printf("Latency (RTT in microseconds):\n");
    printf("  Samples: %" PRIu64 "\n", latency_sample_count);
    printf("  Average: %.3f us\n", sum_latency / latency_sample_count);
    printf("  Min:     %.3f us\n", min_latency);
    printf("  Max:     %.3f us\n", max_latency);
  } else {
    printf("No latency samples recorded (no packets received or matched).\n");
  }
  printf("-------------------------------\n");

  // Send TERMINATE_MSG to server via DPDK
  printf("\nSending termination message to server via DPDK...\n");
  struct rte_mbuf *term_mbuf = rte_pktmbuf_alloc(mbuf_pool);
  if (term_mbuf == NULL) {
    fprintf(stderr, "Failed to allocate mbuf for termination message\n");
  } else {
    construct_udp_packet_with_raw_payload(term_mbuf, TERMINATE_MSG,
                                          strlen(TERMINATE_MSG));
    uint16_t nb_term_tx = rte_eth_tx_burst(port_id, 0, &term_mbuf, 1);
    if (nb_term_tx == 1) {
      printf("Termination message sent successfully.\n");
      // PMD should handle freeing the mbuf on successful transmission
    } else {
      fprintf(stderr, "Failed to send termination message via DPDK.\n");
      rte_pktmbuf_free(term_mbuf); // Free mbuf if send failed
    }
  }

  rte_delay_ms(200); // Give server a moment to process TERMINATE_MSG and set up
                     // TCP listener

  // TCP Client Logic to fetch server statistics
  int tcp_sock_fd;
  struct sockaddr_in server_tcp_addr;
  char server_stats_buffer[256];

  printf("\nAttempting to fetch server statistics via TCP...\n");
  if ((tcp_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("TCP socket creation failed");
  } else {
    memset(&server_tcp_addr, 0, sizeof(server_tcp_addr));
    server_tcp_addr.sin_family = AF_INET;
    server_tcp_addr.sin_port = htons(TCP_PORT); // TCP_PORT from udp_test.h
    if (inet_pton(AF_INET, REMOTE_ADDRESS, &server_tcp_addr.sin_addr) <= 0) {
      perror("Invalid server TCP address / Address not supported");
      close(tcp_sock_fd);
      tcp_sock_fd = -1;
    }

    if (tcp_sock_fd >= 0) {
      printf("Connecting to server on TCP port %d to get stats...\n", TCP_PORT);
      if (connect(tcp_sock_fd, (struct sockaddr *)&server_tcp_addr,
                  sizeof(server_tcp_addr)) < 0) {
        char error_buf[100];
        snprintf(error_buf, sizeof(error_buf), "TCP connect to %s:%d failed",
                 REMOTE_ADDRESS, TCP_PORT);
        perror(error_buf);
      } else {
        printf("TCP connected. Receiving server stats...\n");
        ssize_t bytes_received = recv(tcp_sock_fd, server_stats_buffer,
                                      sizeof(server_stats_buffer) - 1, 0);
        if (bytes_received < 0) {
          perror("TCP recv failed");
        } else if (bytes_received == 0) {
          printf(
              "Server closed TCP connection prematurely (no data received).\n");
        } else {
          server_stats_buffer[bytes_received] = '\0';
          printf("Received raw server stats: \"%s\"\n", server_stats_buffer);

          uint64_t s_msgs_received;
          uint64_t s_total_bytes_received;
          double s_test_duration_reported;

          if (sscanf(server_stats_buffer, "%" SCNu64 " %" SCNu64 " %lf",
                     &s_msgs_received, &s_total_bytes_received,
                     &s_test_duration_reported) == 3) {
            printf("--- Server Statistics (Reported) ---\n");
            printf("  Messages Received by Server: %" PRIu64 "\n",
                   s_msgs_received);
            printf("  Total Bytes Received by Server: %" PRIu64 "\n",
                   s_total_bytes_received);
            if (s_msgs_received > 0) {
              printf("  Avg Message Size at Server: %.2f bytes\n",
                     (double)s_total_bytes_received / s_msgs_received);
            }
            // The server sends its own TEST_DURATION as the third value
            printf("  Server Test Duration Reported: %.2f s\n",
                   s_test_duration_reported);
            printf("------------------------------------\n");
          } else {
            fprintf(stderr,
                    "Failed to parse server statistics string: \"%s\"\n",
                    server_stats_buffer);
          }
        }
      }
      close(tcp_sock_fd);
    }
  }

  printf("\nStopping port %u...\n", port_id);
  rte_eth_dev_stop(port_id);
  rte_eth_dev_close(port_id);
  printf("Cleaning up EAL...\n");
  rte_eal_cleanup();
  printf("Done.\n");

  return 0;
}