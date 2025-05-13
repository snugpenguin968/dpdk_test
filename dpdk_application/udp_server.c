/**
 * This UDP server listens on a fixed port (12345) and echoes back any message
 * it receives.
 */
#include "udp_test.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <signal.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define DPDK_PORT_ID 0 // Port ID of the NIC DPDK uses for the server
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define RX_RING_SIZE 1024
#define TX_RING_SIZE                                                           \
  1024 // Though server doesn't send UDP replies in this version
#define BURST_SIZE 32

static struct rte_mempool *mbuf_pool_server;
static struct rte_ether_addr dpdk_server_eth_addr;

volatile bool force_quit_server = false;

static int port_init_server(uint16_t port_id, struct rte_mempool *pool) {
  struct rte_eth_conf port_conf_default = {
      .rxmode =
          {
              .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
          },
      .txmode =
          {
              .offloads =
                  RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
          },
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
    printf("Error during getting device (port %u) info: %s\n", port_id,
           strerror(-err));
    return err;
  }

  struct rte_eth_conf port_conf = port_conf_default;
  err = rte_eth_dev_configure(port_id, rx_rings, tx_rings, &port_conf);
  if (err != 0) {
    printf("rte_eth_dev_configure failed: err=%d, port=%u\n", err, port_id);
    return err;
  }

  for (q = 0; q < rx_rings; q++) {
    err = rte_eth_rx_queue_setup(port_id, q, RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), NULL, pool);
    if (err < 0) {
      printf("rte_eth_rx_queue_setup failed: err=%d, port=%u\n", err, port_id);
      return err;
    }
  }

  txconf = dev_info.default_txconf;
  txconf.offloads = port_conf.txmode.offloads;
  for (q = 0; q < tx_rings; q++) {
    err = rte_eth_tx_queue_setup(port_id, q, TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), &txconf);
    if (err < 0) {
      printf("rte_eth_tx_queue_setup failed: err=%d, port=%u\n", err, port_id);
      return err;
    }
  }

  err = rte_eth_dev_start(port_id);
  if (err < 0) {
    printf("rte_eth_dev_start failed: err=%d, port=%u\n", err, port_id);
    return err;
  }

  err = rte_eth_macaddr_get(port_id, &dpdk_server_eth_addr);
  if (err != 0) {
    printf("rte_eth_macaddr_get failed: err=%d, port=%u\n", err, port_id);
    return err;
  }
  printf("DPDK Server Port %u MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
         ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 "\n",
         port_id, RTE_ETHER_ADDR_BYTES(&dpdk_server_eth_addr));
  return 0;
}

static void server_signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    printf("Signal %d received on server, preparing to exit...\n", signum);
    force_quit_server = true;
  }
}

int main(int argc, char *argv[]) {
  int err;
  uint16_t port_id = DPDK_PORT_ID;
  unsigned lcore_id;

  // Initialize EAL
  err = rte_eal_init(argc, argv);
  if (err < 0)
    rte_exit(EXIT_FAILURE, "Error with EAL initialization on server\n");

  signal(SIGINT, server_signal_handler);
  signal(SIGTERM, server_signal_handler);

  if (rte_eth_dev_count_avail() == 0)
    rte_exit(EXIT_FAILURE, "No Ethernet ports available for DPDK server\n");

  // Create mbuf pool
  mbuf_pool_server = rte_pktmbuf_pool_create(
      "MBUF_POOL_SERVER", NUM_MBUFS * rte_lcore_count(), MBUF_CACHE_SIZE, 0,
      RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (mbuf_pool_server == NULL)
    rte_exit(EXIT_FAILURE, "Cannot create mbuf pool for server: %s\n",
             rte_strerror(rte_errno));

  // Initialize DPDK Ethernet port
  if (port_init_server(port_id, mbuf_pool_server) != 0)
    rte_exit(EXIT_FAILURE, "Cannot init port %u for DPDK server\n", port_id);

  lcore_id = rte_lcore_id();
  printf("DPDK UDP server starting on lcore %u, listening on port %d via DPDK "
         "NIC\n",
         lcore_id, SERVER_PORT);

  printf("Waiting for link to come up on port %u...\n", port_id);
  struct rte_eth_link link;
  int link_wait_count = 0;
  do {
    rte_eth_link_get_nowait(port_id, &link);
    if (link.link_status)
      break;
    rte_delay_ms(100);
    link_wait_count++;
  } while (link_wait_count < 90 && !force_quit_server);

  if (!link.link_status) {
    printf("Link down on port %u after waiting. Exiting.\n", port_id);
    rte_eal_cleanup();
    return -1;
  }
  printf("Link up on port %u (Speed %u Mbps, %s)\n", port_id, link.link_speed,
         (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "full-duplex"
                                                        : "half-duplex");

  ServerStats stats = {0};
  struct rte_mbuf *rx_bufs[BURST_SIZE];
  uint16_t nb_rx;
  bool terminate_received = false;
  char client_ip_str_from_terminate[INET_ADDRSTRLEN] = "N/A";

  printf("DPDK server listening for UDP packets on port %d...\n", SERVER_PORT);
  while (!force_quit_server && !terminate_received) {
    nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, BURST_SIZE);
    if (nb_rx == 0) {
      continue;
    }
    for (uint16_t i = 0; i < nb_rx; i++) {
      struct rte_mbuf *m = rx_bufs[i];
      struct rte_ether_hdr *eth_h = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
      if (eth_h->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        rte_pktmbuf_free(m);
        continue;
      }
      struct rte_ipv4_hdr *ip_h = (struct rte_ipv4_hdr *)(eth_h + 1);
      if (ip_h->next_proto_id != IPPROTO_UDP) {
        rte_pktmbuf_free(m);
        continue;
      }
      uint16_t ip_hdr_len_bytes = (ip_h->version_ihl & 0x0F) * 4;
      struct rte_udp_hdr *udp_h =
          (struct rte_udp_hdr *)((char *)ip_h + ip_hdr_len_bytes);

      if (udp_h->dst_port == rte_cpu_to_be_16(SERVER_PORT)) {
        char *payload = (char *)(udp_h + 1);
        uint16_t payload_len =
            rte_be_to_cpu_16(udp_h->dgram_len) - sizeof(struct rte_udp_hdr);
        // Check for termination message
        if (payload_len == strlen(TERMINATE_MSG) &&
            strncmp(payload, TERMINATE_MSG, strlen(TERMINATE_MSG)) == 0) {
          struct in_addr src_ip_addr_struct;
          src_ip_addr_struct.s_addr = ip_h->src_addr;
          inet_ntop(AF_INET, &src_ip_addr_struct, client_ip_str_from_terminate,
                    INET_ADDRSTRLEN);
          printf("Termination message received from %s. Preparing TCP stats "
                 "transfer...\n",
                 client_ip_str_from_terminate);
          terminate_received = true;
        } else {
          stats.messages_received++;
          stats.total_bytes_received += payload_len;

          struct rte_mbuf *echo_mbuf = rte_pktmbuf_alloc(mbuf_pool_server);
          if (echo_mbuf) {
            // Construct the echo packet
            // Ethernet header
            struct rte_ether_hdr *orig_eth_h =
                rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            struct rte_ether_hdr *echo_eth_h =
                rte_pktmbuf_mtod(echo_mbuf, struct rte_ether_hdr *);
            rte_ether_addr_copy(
                &orig_eth_h->src_addr,
                &echo_eth_h->dst_addr); // Dest is original source
            rte_ether_addr_copy(
                &dpdk_server_eth_addr,
                &echo_eth_h->src_addr); // Source is server's MAC
            echo_eth_h->ether_type = orig_eth_h->ether_type;

            // IP header
            struct rte_ipv4_hdr *orig_ip_h =
                (struct rte_ipv4_hdr *)(orig_eth_h + 1);
            struct rte_ipv4_hdr *echo_ip_h =
                (struct rte_ipv4_hdr *)(echo_eth_h + 1);
            // Most fields can be copied, then swap src/dst IP
            memcpy(echo_ip_h, orig_ip_h, sizeof(struct rte_ipv4_hdr));
            echo_ip_h->src_addr =
                orig_ip_h->dst_addr; // Server's IP (originally dest)
            echo_ip_h->dst_addr =
                orig_ip_h->src_addr;     // Client's IP (originally src)
            echo_ip_h->hdr_checksum = 0; // Recalculate

            // UDP header
            uint16_t orig_ip_hdr_len_bytes =
                (orig_ip_h->version_ihl & 0x0F) * 4;
            struct rte_udp_hdr *orig_udp_h =
                (struct rte_udp_hdr *)((char *)orig_ip_h +
                                       orig_ip_hdr_len_bytes);
            struct rte_udp_hdr *echo_udp_h =
                (struct rte_udp_hdr
                     *)((char *)echo_ip_h +
                        orig_ip_hdr_len_bytes); // Assuming same IP hdr len
            // Most fields can be copied, then swap src/dst ports
            memcpy(echo_udp_h, orig_udp_h, sizeof(struct rte_udp_hdr));
            echo_udp_h->src_port =
                orig_udp_h->dst_port; // Server's port (originally dest)
            echo_udp_h->dst_port =
                orig_udp_h->src_port;    // Client's port (originally src)
            echo_udp_h->dgram_cksum = 0; // Recalculate

            // Copy payload
            char *orig_payload = (char *)(orig_udp_h + 1);
            uint16_t orig_payload_len =
                rte_be_to_cpu_16(orig_udp_h->dgram_len) -
                sizeof(struct rte_udp_hdr);
            char *echo_payload = (char *)(echo_udp_h + 1);
            rte_memcpy(echo_payload, orig_payload, orig_payload_len);

            // Set mbuf lengths and offload flags for the echo packet
            echo_mbuf->data_len =
                m->data_len; // Assuming total length is the same
            echo_mbuf->pkt_len = m->pkt_len;
            echo_mbuf->l2_len = sizeof(struct rte_ether_hdr);
            echo_mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
            echo_mbuf->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM |
                                  RTE_MBUF_F_TX_UDP_CKSUM;

            // Send the echo packet
            uint16_t nb_echo_tx = rte_eth_tx_burst(port_id, 0, &echo_mbuf, 1);
            if (nb_echo_tx == 0) {
              rte_pktmbuf_free(echo_mbuf); // Free if send failed
            }
          }
        }
        rte_pktmbuf_free(m);
        if (terminate_received)
          break;
      }
    }

    if (force_quit_server && !terminate_received) {
      printf("Server shutting down due to signal before receiving termination "
             "message.\n");
      // No TCP stats transfer in this case
    }

    if (stats.messages_received > 0) {
      stats.avg_message_size =
          (double)stats.total_bytes_received / stats.messages_received;
    } else {
      stats.avg_message_size = 0.0;
    }
    printf("UDP processing finished. Messages: %" PRIu64 ", Bytes: %" PRIu64
           ", Avg Size: %.2f\n",
           stats.messages_received, stats.total_bytes_received,
           stats.avg_message_size);

    // Transmit server statistics to client using separate TCP connection
    if (terminate_received) {
      printf("\nSetting up TCP server to send statistics on port %d...\n",
             TCP_PORT);
      int tcp_listen_sockfd, tcp_conn_sockfd;
      struct sockaddr_in tcp_servaddr, tcp_cliaddr;
      socklen_t tcp_cliaddr_len = sizeof(tcp_cliaddr);

      if ((tcp_listen_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP listen socket creation failed");
      } else {
        memset(&tcp_servaddr, 0, sizeof(tcp_servaddr));
        tcp_servaddr.sin_family = AF_INET;
        tcp_servaddr.sin_addr.s_addr =
            INADDR_ANY; // Listen on all kernel-managed interfaces
        tcp_servaddr.sin_port = htons(TCP_PORT);

        if (bind(tcp_listen_sockfd, (const struct sockaddr *)&tcp_servaddr,
                 sizeof(tcp_servaddr)) < 0) {
          perror("TCP bind failed");
        } else {
          if (listen(tcp_listen_sockfd, 1) < 0) {
            perror("TCP listen failed");
          } else {
            printf("TCP server waiting for connection from client %s (or any "
                   "client) on port %d...\n",
                   client_ip_str_from_terminate, TCP_PORT);
            if ((tcp_conn_sockfd =
                     accept(tcp_listen_sockfd, (struct sockaddr *)&tcp_cliaddr,
                            &tcp_cliaddr_len)) < 0) {
              perror("TCP accept failed");
            } else {
              char connected_client_ip[INET_ADDRSTRLEN];
              inet_ntop(AF_INET, &tcp_cliaddr.sin_addr, connected_client_ip,
                        INET_ADDRSTRLEN);
              printf("TCP connection accepted from %s:%d\n",
                     connected_client_ip, ntohs(tcp_cliaddr.sin_port));

              char stats_buffer_tcp[256];
              // Format matches what client expects: msg_rcv, bytes_rcv,
              // TEST_DURATION
              snprintf(stats_buffer_tcp, sizeof(stats_buffer_tcp),
                       "%" PRIu64 " %" PRIu64 " %.6f", stats.messages_received,
                       stats.total_bytes_received, TEST_DURATION);

              ssize_t sent_bytes =
                  send(tcp_conn_sockfd, stats_buffer_tcp,
                       strlen(stats_buffer_tcp) + 1, 0); // +1 for null
              if (sent_bytes < 0) {
                perror("TCP send failed");
              } else {
                printf("Sent statistics to client via TCP: \"%s\"\n",
                       stats_buffer_tcp);
              }
              close(tcp_conn_sockfd);
            }
          }
        }
        close(tcp_listen_sockfd);
      }
    }

    // Cleanup DPDK
    printf("\nStopping DPDK port %u...\n", port_id);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    printf("Cleaning up EAL on server...\n");
    rte_eal_cleanup();
    printf("DPDK Server shut down.\n");

    return 0;
  }
}