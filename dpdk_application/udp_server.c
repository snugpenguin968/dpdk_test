/**
 * This UDP server listens on a fixed port (12345) and echoes back any message
 * it receives.
 */
#include "udp_test.h" 
#include <inttypes.h>
#include <ctype.h>
#include <signal.h>
#include <arpa/inet.h> /

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>

#define DPDK_PORT_ID 0      // Port ID of the NIC DPDK uses for the server
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024   // Though server doesn't send UDP replies in this version
#define BURST_SIZE 32

static struct rte_mempool *mbuf_pool_server;
static struct rte_ether_addr dpdk_server_eth_addr;

volatile bool force_quit_server = false;

static int
port_init_server(uint16_t port_id, struct rte_mempoll *pool) {
  struct rte_eth_conf port_conf_default = {
    .rxmode = {
      .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
    },
    .txmode = {
      .offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
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
    printf("Error during getting device (port %u) info: %s\n", port_id, strerror(-err));
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