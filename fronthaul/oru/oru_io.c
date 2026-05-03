/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "oru_io.h"
#include <rte_ether.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_errno.h>
#include "assertions.h"

#define ECPRI_ETHER_TYPE 0xAEFE

static int configure_ru_flows(uint16_t port_id, int num_du_macs, struct rte_ether_addr *du_macs)
{
  AssertFatal(num_du_macs > 0 && num_du_macs <= 2,
              "Invalid number of DU MACs. Expected one MAC for UPlane & CPlane or 2 separate MACs\n");
  struct rte_flow_error error;
  struct rte_flow_attr attr = {.ingress = 1};

  // Pattern 1: Match O-DU MAC 1 + eCPRI
  struct rte_flow_item pattern[3];
  struct rte_flow_item_eth eth_spec, eth_mask;

  memset(pattern, 0, sizeof(pattern));
  memset(&eth_spec, 0, sizeof(eth_spec));
  memset(&eth_mask, 0, sizeof(eth_mask));

  // We match Source MAC == DU MAC 1 and EtherType == 0xAEFE
  rte_ether_addr_copy(&du_macs[0], &eth_spec.src);
  eth_spec.type = rte_cpu_to_be_16(ECPRI_ETHER_TYPE);

  memset(&eth_mask.src, 0xFF, 6);
  eth_mask.type = 0xFFFF;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[0].spec = &eth_spec;
  pattern[0].mask = &eth_mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  // Action: Queue 0
  struct rte_flow_action actions[2];
  struct rte_flow_action_queue queue = {.index = 0};

  actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  actions[0].conf = &queue;
  actions[1].type = RTE_FLOW_ACTION_TYPE_END;

  struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
  if (!flow) {
    fprintf(stderr, "Failed to create flow for DU MAC 1 on port %u: %s\n", port_id, error.message);
    return -1;
  }

  // If second MAC configured, add second rule
  if (num_du_macs == 2) {
    rte_ether_addr_copy(&du_macs[1], &eth_spec.src);
    flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
      fprintf(stderr, "Failed to create flow for DU MAC 2 on port %u: %s\n", port_id, error.message);
    }
  }

  return 0;
}

static int configure_dpdk_port(uint16_t port_id, oru_io_config_t *conf, struct rte_mempool *pool)
{
  struct rte_eth_conf port_conf = {
      .rxmode =
          {
              .mq_mode = RTE_ETH_MQ_RX_NONE,
          },
  };

  int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
  if (ret < 0) {
    fprintf(stderr, "Failed to configure port %u: %s\n", port_id, rte_strerror(-ret));
    return ret;
  }

  ret = rte_eth_rx_queue_setup(port_id, 0, 1024, rte_eth_dev_socket_id(port_id), NULL, pool);
  if (ret < 0) {
    fprintf(stderr, "Failed to setup RX queue 0 on port %u: %s\n", port_id, rte_strerror(-ret));
    return ret;
  }

  ret = rte_eth_tx_queue_setup(port_id, 0, 1024, rte_eth_dev_socket_id(port_id), NULL);
  if (ret < 0) {
    fprintf(stderr, "Failed to setup TX queue 0 on port %u: %s\n", port_id, rte_strerror(-ret));
    return ret;
  }

  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    fprintf(stderr, "Failed to start port %u: %s\n", port_id, rte_strerror(-ret));
    return ret;
  }

  return 0;
}

int oru_io_init(oru_io_t *io, oru_io_config_t *conf)
{
  if (!io || !conf || conf->num_ports == 0)
    return -1;
  io->conf = *conf;

  // 1. Create Mbuf Pool if requested
  if (conf->mbuf_count > 0) {
    io->rx_pool = rte_pktmbuf_pool_create("oru_rx_pool", conf->mbuf_count, 256, 0, conf->mbuf_data_room, rte_socket_id());
    if (!io->rx_pool) {
      fprintf(stderr, "Failed to create mbuf pool: %s\n", rte_strerror(rte_errno));
      return -1;
    }
    io->tx_pool = rte_pktmbuf_pool_create("oru_tx_pool", conf->mbuf_count, 256, 0, conf->mbuf_data_room, rte_socket_id());
    if (!io->tx_pool) {
      fprintf(stderr, "Failed to create mbuf pool: %s\n", rte_strerror(rte_errno));
      return -1;
    }
    io->mbuf_size = conf->mbuf_data_room; // Record data size for fragmentation awareness
  }

  // 2. Configure Ports
  for (uint16_t i = 0; i < conf->num_ports; i++) {
    if (configure_dpdk_port(conf->port_ids[i], conf, io->rx_pool) < 0)
      return -1;

    rte_eth_macaddr_get(conf->port_ids[i], &io->local_macs[i]);
    if (rte_eth_dev_get_mtu(conf->port_ids[i], &io->mtus[i]) < 0) {
      fprintf(stderr, "Failed to get MTU for port %u, defaulting to 1500\n", conf->port_ids[i]);
      io->mtus[i] = 1500;
    }
    printf("ORU_IO: Port %u: MAC %02X:%02X:%02X:%02X:%02X:%02X, MTU %u\n",
           conf->port_ids[i],
           io->local_macs[i].addr_bytes[0],
           io->local_macs[i].addr_bytes[1],
           io->local_macs[i].addr_bytes[2],
           io->local_macs[i].addr_bytes[3],
           io->local_macs[i].addr_bytes[4],
           io->local_macs[i].addr_bytes[5],
           io->mtus[i]);
  }

  // 3. Initialize Timer
  if (fh_timer_init(&io->timer, conf->numerology) < 0)
    return -1;
  fh_timer_register_cb(&io->timer, conf->timer_cb, conf->timer_user_data);

  // 4. Initialize Send (on primary port)
  if (fh_send_init(&io->send, &io->timer, conf->port_ids[0], 0) < 0)
    return -1;

  // 5. Initialize Recv (on all configured ports)
  if (conf->num_ports == 1) {
    // Configure all configured MACs for 1 port
    fh_recv_q_conf_t rx_q;
    rx_q.port_id = conf->port_ids[0];
    rx_q.queue_id = 0;
    configure_ru_flows(conf->port_ids[0], conf->num_macs, conf->du_macs);
    if (fh_recv_init(&io->recv, &io->timer, &rx_q, 1, conf->rx_cb, conf->rx_user_data) < 0)
      return -1;
  } else if (conf->num_ports == 2) {
    fh_recv_q_conf_t rx_qs[MAX_RU_PORTS];
    for (uint16_t i = 0; i < conf->num_ports; i++) {
      struct rte_ether_addr *du_mac = &conf->du_macs[i];
      if (i == 1 && conf->num_macs == 1) {
        du_mac = &conf->du_macs[0];
      }
      rx_qs[i].port_id = conf->port_ids[i];
      rx_qs[i].queue_id = 0;

      // 6. Configure Flows
      configure_ru_flows(conf->port_ids[i], 1, du_mac);
    }
    if (fh_recv_init(&io->recv, &io->timer, rx_qs, conf->num_ports, conf->rx_cb, conf->rx_user_data) < 0)
      return -1;
  } else {
    fprintf(stderr, "Unsupported number of ports: %d\n", conf->num_ports);
    return -1;
  }
  return 0;
}

int oru_io_send_uplane(oru_io_t *io, struct rte_mbuf **mbufs, uint32_t num_mbufs)
{
  for (uint32_t i = 0; i < num_mbufs; i++) {
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)rte_pktmbuf_prepend(mbufs[i], sizeof(struct rte_ether_hdr));
    if (!eth)
      continue;

    eth->ether_type = rte_cpu_to_be_16(ECPRI_ETHER_TYPE);
    rte_ether_addr_copy(&io->conf.du_macs[0], &eth->dst_addr);
    rte_ether_addr_copy(&io->local_macs[0], &eth->src_addr);
  }

  return fh_send_immediate(&io->send, mbufs, num_mbufs);
}

void oru_io_register_timer_cb(oru_io_t *io, fh_timer_cb cb, void *user_data)
{
  if (!io)
    return;
  fh_timer_register_cb(&io->timer, cb, user_data);
}

int oru_io_run(oru_io_t *io, unsigned int worker_lcore)
{
  return rte_eal_remote_launch(fh_recv_run, &io->recv, worker_lcore);
}

void oru_io_stop(oru_io_t *io)
{
  fh_timer_stop(&io->timer);
  fh_recv_stop(&io->recv);
}

void oru_io_cleanup(oru_io_t *io)
{
  if (io->rx_pool) {
    rte_mempool_free(io->rx_pool);
    io->rx_pool = NULL;
  }
  if (io->tx_pool) {
    rte_mempool_free(io->tx_pool);
    io->tx_pool = NULL;
  }
}

struct rte_mbuf *oru_io_get_sendbuf(oru_io_t *io)
{
  return rte_pktmbuf_alloc(io->tx_pool);
}
