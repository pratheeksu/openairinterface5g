/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief gNB PCCH (paging) scheduling procedures
 */

#include <stdbool.h>
#include <stdint.h>
#include "assertions.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "NR_MAC_gNB/mac_proto.h"
#include "common/utils/LOG/log.h"
#include "common/utils/ds/byte_array.h"
#include "openair2/RRC/NR/MESSAGES/asn1_msg.h"

/** @brief Drop the oldest SDU from the queue. */
static void pcch_queue_drop_head(nr_mac_pcch_queue_t *q, int head_idx)
{
  DevAssert(q != NULL);
  DevAssert(q->count > 0);
  free(q->items[head_idx].sdu);
  q->items[head_idx].sdu_len = 0;
  q->items[head_idx].ue_id = 0;
  q->head = (q->head + 1) % NR_PCCH_MAX_PAGING_RECORDS;
  q->count--;
}

/** @brief Push a PCCH SDU into the queue. */
static void pcch_queue_push(nr_mac_pcch_queue_t *q, module_id_t module_id, const byte_array_t *sdu, uint16_t ue_id)
{
  DevAssert(q != NULL);
  DevAssert(sdu != NULL);
  DevAssert(sdu->buf != NULL);
  DevAssert(sdu->len > 0);

  if (q->count == NR_PCCH_MAX_PAGING_RECORDS) {
    const int drop = q->head;
    pcch_queue_drop_head(q, drop);
    LOG_W(NR_MAC, "[gNB %d] PCCH queue full, dropping oldest SDU\n", module_id);
  }

  const int idx = (q->head + q->count) % NR_PCCH_MAX_PAGING_RECORDS;
  DevAssert(q->items[idx].sdu == NULL);
  q->items[idx].sdu = malloc_or_fail(sdu->len);
  memcpy(q->items[idx].sdu, sdu->buf, sdu->len);
  q->items[idx].sdu_len = sdu->len;
  q->items[idx].ue_id = ue_id % 1024;
  q->count++;
}

/** @brief Build NR Paging (PCCH) SDU for ng-5G-S-TMSI and enqueue it in the MAC PCCH queue. */
void nr_mac_pcch_enqueue(module_id_t module_id, uint32_t m_tmsi, uint16_t ue_id)
{
  gNB_MAC_INST *mac = RC.nrmac[module_id];
  DevAssert(mac);
  const int CC_id = 0;
  NR_COMMON_channels_t *cc = &mac->common_channels[CC_id];

  nr_paging_params_t params = {
      .ue_identity_type = NR_PagingUE_Identity_PR_ng_5G_S_TMSI,
      .ue_identity = {.m_tmsi = m_tmsi},
      .access_type = false,
  };
  byte_array_t msg = do_NR_Paging(&params);
  if (!msg.buf || msg.len == 0) {
    LOG_E(NR_MAC, "[gNB %d] PCCH paging: do_NR_Paging failed for m_tmsi=0x%08x\n", module_id, m_tmsi);
    return;
  }

  NR_SCHED_LOCK(&mac->sched_lock);
  pcch_queue_push(&cc->pcch_queue, module_id, &msg, ue_id);
  NR_SCHED_UNLOCK(&mac->sched_lock);

  LOG_I(NR_MAC, "[gNB %d] PCCH SDU enqueued length=%zu UE_ID=%u (m_tmsi=0x%08x)\n", module_id, msg.len, ue_id, m_tmsi);
  free_byte_array(msg);
}
