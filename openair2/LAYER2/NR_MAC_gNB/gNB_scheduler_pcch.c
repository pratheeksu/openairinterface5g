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
#include "NR_PCCH-Config.h"

/** @brief Drop the oldest SDU from the queue. */
static void pcch_queue_drop_head(nr_mac_pcch_queue_t *q, int head_idx)
{
  DevAssert(q != NULL);
  DevAssert(q->count > 0);
  free(q->items[head_idx].sdu);
  q->items[head_idx].sdu = NULL;
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

/** @brief Get paging search space (Type2-PDCCH CSS) from common search space list by pagingSearchSpace ID.
 * TS 38.331 PDCCH-ConfigCommon: pagingSearchSpace is the ID of the search space for paging
 * on the initial DL BWP. If this field is absent, the UE does not receive paging on this BWP
 * (TS 38.213 §10), so we do not schedule PCCH here either.
 * @return the SearchSpace from commonSearchSpaceList whose searchSpaceId matches the SS referenced
 * by the pagingSearchSpace ID and whose searchSpaceType is common, or NULL otherwise. */
static NR_SearchSpace_t *get_paging_search_space(NR_ServingCellConfigCommon_t *scc)
{
  NR_PDCCH_ConfigCommon_t *pdcch = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup;
  if (!pdcch->pagingSearchSpace || !pdcch->commonSearchSpaceList)
    return NULL;
  NR_SearchSpaceId_t id = *pdcch->pagingSearchSpace;
  for (int i = 0; i < pdcch->commonSearchSpaceList->list.count; i++) {
    NR_SearchSpace_t *ss = pdcch->commonSearchSpaceList->list.array[i];
    if (ss->searchSpaceId != id)
      continue;
    if (ss->searchSpaceType == NULL || ss->searchSpaceType->present != NR_SearchSpace__searchSpaceType_PR_common)
      return NULL;
    return ss;
  }
  return NULL;
}

/** @brief True if (frame, slot) is a Type0-PDCCH monitoring occasion (TS 38.213 Clause 13).
 *
 * Type0-PDCCH CSS carries RMSI (SIB1). When SearchSpaceId = 0 for pagingSearchSpace, TS 38.304
 * requires paging PDCCH monitoring occasions to be the same as for RMSI, so we reuse this check
 * for Ns=1 paging PO slot selection. */
static bool is_type0_occasion_paging(gNB_MAC_INST *mac, NR_ServingCellConfigCommon_t *scc, uint16_t frame, uint16_t slot)
{
  DevAssert(mac);
  DevAssert(scc);
  const int L_max = get_max_ssbs(scc);
  // Loops over all SSB indices, returns true if (frame, slot) is the Type0-PDCCH CSS monitoring occasion for any SSB
  for (int i = 0; i < L_max; i++) {
    if (is_type0_occasion(scc, &mac->type0_PDCCH_CSS_config[i], frame, slot))
      return true;
  }
  return false;
}

/** @brief Build NFAPI DL TTI PDUs (PDCCH + PDSCH) for PCCH with P-RNTI.
 *
 * Builds:
 * - PDCCH PDU carrying DCI format 1_0 with CRC scrambled by P-RNTI (TS 38.212 §7.3.1.2.1) in the
 *   Type2-PDCCH common search space configured by pagingSearchSpace (TS 38.213 §10.1).
 * - PDSCH PDU carrying the Paging RRC PDU (PCCH-Message, TS 38.331).
 * NFAPI envelopes follow SCF NFAPI DL_TTI.request (PDCCH/PDSCH PDU types).
 *
 * @param mac              gNB MAC instance
 * @param pdsch            PDSCH allocation parameters (TDA, BWP, MCS, RB allocation, TBS)
 * @param pdcch            PDCCH allocation parameters (CCE/REG mapping, BWP)
 * @param search_space     Paging search space (Type2-PDCCH CSS)
 * @param coreset          CORESET referenced by the search space
 * @param aggregation_level PDCCH aggregation level (TS 38.213 §10.1, Table 10.1-1)
 * @param cce_index        First CCE index of the chosen candidate
 * @param dl_req           DL TTI request body to fill (PDCCH+PDSCH PDUs)
 * @param pdu_index        TX_DATA PDU index linking PDSCH grant to payload
 * @param beam_index       Internal beam index */
static void nr_fill_nfapi_dl_PCCH_pdu(gNB_MAC_INST *mac,
                                      NR_sched_pdsch_t *pdsch,
                                      NR_sched_pdcch_t *pdcch,
                                      NR_SearchSpace_t *search_space,
                                      NR_ControlResourceSet_t *coreset,
                                      int aggregation_level,
                                      int cce_index,
                                      nfapi_nr_dl_tti_request_body_t *dl_req,
                                      int pdu_index,
                                      int beam_index)
{
  const int CC_id = 0;
  NR_COMMON_channels_t *cc = &mac->common_channels[CC_id];
  NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;
  const uint16_t pdcch_pdu_size = 4 + sizeof(nfapi_nr_dl_tti_pdcch_pdu);
  const uint16_t pdsch_pdu_size = 4 + sizeof(nfapi_nr_dl_tti_pdsch_pdu);

  nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdcch_pdu = &dl_req->dl_tti_pdu_list[dl_req->nPDUs];
  memset(dl_tti_pdcch_pdu, 0, sizeof(*dl_tti_pdcch_pdu));
  dl_tti_pdcch_pdu->PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
  dl_tti_pdcch_pdu->PDUSize = pdcch_pdu_size;
  dl_req->nPDUs += 1;
  nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu_rel15 = &dl_tti_pdcch_pdu->pdcch_pdu.pdcch_pdu_rel15;
  nr_configure_pdcch(pdcch_pdu_rel15, coreset, pdcch);

  nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdsch_pdu = &dl_req->dl_tti_pdu_list[dl_req->nPDUs];
  memset(dl_tti_pdsch_pdu, 0, sizeof(*dl_tti_pdsch_pdu));
  dl_tti_pdsch_pdu->PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
  dl_tti_pdsch_pdu->PDUSize = pdsch_pdu_size;
  dl_req->nPDUs += 1;

  const uint16_t fapi_beam = convert_to_fapi_beam(beam_index, mac->beam_info.beam_mode);
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *pdsch_pdu_rel15 =
      prepare_pdsch_pdu(dl_tti_pdsch_pdu, mac, NULL, pdsch, NULL, false, 0, P_RNTI, fapi_beam, 1, pdu_index);

  /* Fill PDCCH DL DCI PDU for paging (P-RNTI, DCI 1_0, TYPE_P_RNTI_).
   * prepare_dci_dl_payload() sets short_messages_indicator = 01 (TS 38.212 Table 7.3.1.2.1-1:
   * "Only scheduling information for Paging is present") since we deliver a Paging PDU on PDSCH and
   * do not signal a Short Message in the DCI. */
  nfapi_nr_dl_dci_pdu_t *dci_pdu =
      prepare_dci_pdu(pdcch_pdu_rel15, scc, search_space, coreset, aggregation_level, cce_index, fapi_beam, P_RNTI);
  pdcch_pdu_rel15->numDlDci++;

  const nr_dci_format_t dci_format = NR_DL_DCI_FORMAT_1_0;
  const nr_rnti_type_t rnti_type = TYPE_P_RNTI_;
  dci_pdu_rel15_t dci_payload = prepare_dci_dl_payload(mac,
                                                       NULL,
                                                       rnti_type,
                                                       NR_SearchSpace__searchSpaceType_PR_common,
                                                       pdsch_pdu_rel15,
                                                       pdsch,
                                                       NULL,
                                                       0,
                                                       0,
                                                       0,
                                                       false);

  fill_dci_pdu_rel15(NULL, NULL, NULL, dci_pdu, &dci_payload, dci_format, rnti_type, search_space, coreset, 0, mac->cset0_bwp_size);
}

/** @brief Returns true if (frame, slot) is a paging occasion (PO) for a given UE.
 *
 * Implements TS 38.304 §7.1 PF/PO determination and the mapping to PDCCH
 * monitoring occasions based on the configured paging search space.
 *
 * Scope: CN-initiated paging in RRC_IDLE only (UE_ID = 5G-S-TMSI mod 1024).
 *
 * - PF (Paging Frame) condition: (SFN + PF_offset) mod T = (T / N) * (UE_ID mod N)
 * - PO (Paging Occasion) index:  i_s = floor(UE_ID / N) mod Ns
 *   where T, N, Ns and PF_offset are derived from PCCH-Config (SIB1), and
 *   UE_ID is 5G-S-TMSI mod 1024.
 *
 * For SearchSpaceId = 0, Ns is 1 or 2 and this function reuses the Type0-PDCCH
 * (RMSI) timing per TS 38.213 §13 (Type0 CSS) to decide the slot(s) belonging
 * to the PO. For SearchSpaceId != 0 (Type2), the PO is S*X consecutive PDCCH
 * monitoring occasions (MOs), where S is the number of actually transmitted
 * SSBs (derived from ssb-PositionsInBurst in SIB1) and X is the number of
 * PDCCH MOs per SSB in the PO (nrofPDCCH-MonitoringOccasionPerSSB-InPO if
 * configured, otherwise X = 1). The [x*S+K]-th MO in the PO maps to the K-th
 * transmitted SSB, per TS 38.304 §7.1.
 *
 * A PO (frame, slot) is valid if it is a MO of the paging search space and its
 * index within the PF falls in the prescribed range. When configured,
 * firstPDCCH-MonitoringOccasionOfPO provides the start MO index of the
 * (i_s+1)-th PO, otherwise it defaults to i_s*S*X (the first PO).
 *
 * @param pcch           PCCH-Config from SIB1 (N, PF_offset, Ns, T)
 * @param frame          System frame number
 * @param slot           Slot index within the frame
 * @param ue_id          UE_ID used in computation of PF/PO (5G-S-TMSI mod 1024)
 * @param mac            pointer to gNB MAC instance
 * @param scc            pointer to ServingCellConfigCommon
 * @return true if (frame, slot) is a paging occasion for ue_id, false otherwise. */
static bool is_paging_occasion(const NR_PCCH_Config_t *pcch,
                               uint16_t frame,
                               uint16_t slot,
                               uint16_t ue_id,
                               gNB_MAC_INST *mac,
                               NR_ServingCellConfigCommon_t *scc)
{
  DevAssert(pcch);
  DevAssert(mac);
  DevAssert(scc);

  /* TS 38.213 §10.1 (Type2-PDCCH CSS on the primary cell of the MCG, i.e. the PCell) /
   * TS 38.331 PDCCH-ConfigCommon: If a UE is not provided pagingSearchSpace for Type2-PDCCH
   * CSS set, the UE does not monitor PDCCH for Type2-PDCCH CSS set on the DL BWP. */
  NR_SearchSpace_t *ss = get_paging_search_space(scc);
  if (ss == NULL) {
    LOG_D(NR_MAC, "[%04d.%02d][gNB %d] No pagingSearchSpace configured for PCCH (UE_ID=%u)\n", frame, slot, mac->Mod_id, ue_id);
    return false;
  }

  /* T: DRX cycle of the UE in radio frames (TS 38.304 §7.1). */
  const uint16_t T = nr_pcch_default_paging_cycle_rf(pcch);
  uint16_t N;
  uint8_t PF_offset;
  /** N and PF_offset: derived from nAndPagingFrameOffset (TS 38.331 PCCH-Config), per TS 38.304 §7.1.
   * N spreads UEs across the T radio frames so that each UE has exactly
   * one PF per DRX cycle. Larger N means more PFs per cycle. It is encoded in nAndPagingFrameOffset.
   * Valid N depends on pagingSearchSpace, SS/PBCH block and CORESET multiplexing pattern (TS 38.213),
   * and ssb-periodicityServingCell: this function uses the configured value. Validity is enforced
   * when building SIB1 / PCCH-Config. */
  nr_pcch_n_and_paging_frame_offset(pcch, T, &N, &PF_offset);
  /* Ns: number of paging occasions per paging frame (TS 38.331 PCCH-Config). */
  const uint8_t Ns = nr_pcch_ns_per_pf(pcch);

  /* TS 38.304 §7.1 Paging Frame condition for which this SFN is a PF for ue_id */
  if (!nr_pcch_sfn_is_pf(frame, PF_offset, T, N, ue_id)) {
    LOG_D(NR_MAC,
          "[%04d.%02d][gNB %d] not in PF (UE_ID=%u T=%u N=%u PF_offset=%u)\n",
          frame,
          slot,
          mac->Mod_id,
          ue_id,
          T,
          N,
          PF_offset);
    return false;
  }

  /* TS 38.304 §7.1 Paging Occasion index in this PF
   * The UE then monitors the (i_s + 1)-th PO within this PF. */
  const uint8_t i_s = nr_pcch_po_index(ue_id, N, Ns);
  const long ss_id = ss->searchSpaceId;
  LOG_D(NR_MAC,
        "[%04d.%03d] [gNB %d] in PF for UE_ID=%u (i_s=%u Ns=%u ss_id=%ld)\n",
        frame,
        slot,
        mac->Mod_id,
        ue_id,
        i_s,
        Ns,
        ss_id);

  /* TS 38.304 §7.1 + TS 38.213 §10.1: PO slot timing within the PF depends on pagingSearchSpace (SearchSpaceId).
   *
   * If pagingSearchSpace identifies SearchSpaceId = 0 (TS 38.213 §10.1): the UE uses PDCCH monitoring
   * occasions as for Type0/RMSI (TS 38.213 Clause 13), same occasions as for RMSI. TS 38.304 §7.1 then restricts
   * Ns to 1 or 2 only for this case:
   *   Ns = 1: PO starts at the first paging MO in the PF, delegate to Type0 occasion check.
   *   Ns = 2: PO occupies the first or second half of the PF according to i_s (0 or 1).
   *
   * If SearchSpaceId != 0 (e.g. 2): Type2-PDCCH CSS per §10.1. Paging timing is based on
   * firstPDCCH-MonitoringOccasionOfPO and nrofPDCCH-MonitoringOccasionPerSSB-InPO. PO spans
   * S*X consecutive MOs (handled below). */
  if (ss_id == 0) {
    /* TS 38.304 §7.1: When SearchSpaceId = 0 for pagingSearchSpace, Ns is either 1 or 2. */
    AssertFatal(Ns <= 2, "TS 38.304 §7.1: Ns=%u invalid when pagingSearchSpace is SearchSpaceId 0 (must be 1 or 2)\n", Ns);

    /* Ns = 1: single PO in the PF, starting at the first PDCCH MO for paging (same MOs as RMSI, TS 38.213 §13). */
    if (Ns == 1)
      return is_type0_occasion_paging(mac, scc, frame, slot);

    /* Ns = 2: PO in first half (i_s = 0) or second half (i_s = 1) of the PF (TS 38.304 §7.1). */
    if (Ns == 2) {
      const int n_slots_frame = mac->frame_structure.numb_slots_frame;
      return nr_pcch_ss0_po_half_frame(i_s, slot, n_slots_frame);
    }
    return false;
  } else {
    /* Type2 paging (SearchSpaceId != 0), TS 38.304 §7.1, TS 38.331 PCCH-Config, TS 38.213 §10.1:
     * The (i_s+1)-th PO is S*X consecutive PDCCH monitoring occasions (MOs) in the PF.
     * - S = number of actually transmitted SSBs (ssb-PositionsInBurst in SIB1, TS 38.304 §7.1)
     * - X = nrofPDCCH-MonitoringOccasionPerSSB-InPO if present, else 1
     * - firstPDCCH-MonitoringOccasionOfPO: when present, (i_s+1)th entry gives start_mo; when absent,
     *   default is start_mo = i_s*S*X (TS 38.331)
     * - MO slots follow monitoringSlotPeriodicityAndOffset (TS 38.213 §10.1): slot indices offset + k*period.
     * - Simplifications:
     *   - MOs that overlap UL symbols per tdd-UL-DL-ConfigurationCommon are not excluded from numbering (TS 38.304 §7.1)
     *   - PO evaluation is PF-local only (a PO may span multiple frames / search-space periods).
     * - MO indices within the PF are numbered from the first paging MO slot in this frame: current slot must fall in
     *   [start_mo, end_mo].
     *
     * Note: this implementation assumes one paging MO per matched slot (typical for duration=1 and one
     * monitoringSymbolsWithinSlot group). For other cases (duration>1 or multiple groups), the MO index
     * calculation needs to be adjusted. */
    int period = 0;
    int offset = 0;
    get_monitoring_period_offset(ss, &period, &offset);
    const int n_slots_frame = mac->frame_structure.numb_slots_frame;

    uint8_t ssb_len = 0;
    const uint64_t ssb_bmp = get_ssb_bitmap_and_len(scc, &ssb_len);
    uint64_t ssb_bits = ssb_bmp >> (64 - ssb_len);
    int S = 0;
    while (ssb_bits) {
      S += ssb_bits & 1u;
      ssb_bits >>= 1;
    }
    AssertFatal(S > 0, "ssb-PositionsInBurst has no transmitted SSB (S must be >= 1 for paging)\n");
    int X = 1;
    if (pcch->ext1 != NULL && pcch->ext1->nrofPDCCH_MonitoringOccasionPerSSB_InPO_r16 != NULL) {
      X = *pcch->ext1->nrofPDCCH_MonitoringOccasionPerSSB_InPO_r16;
      AssertFatal(X >= 2 && X <= NR_PCCH_MAX_MO_PER_SSB_IN_PO,
                  "TS 38.331 PCCH-Config: nrofPDCCH-MonitoringOccasionPerSSB-InPO-r16=%d out of range (2..%d)\n",
                  X,
                  NR_PCCH_MAX_MO_PER_SSB_IN_PO);
    }
    int start_mo = i_s * S * X;
    if (pcch->firstPDCCH_MonitoringOccasionOfPO)
      nr_pcch_first_pdcch_start_mo(pcch->firstPDCCH_MonitoringOccasionOfPO, i_s, &start_mo);
    int end_mo = start_mo + S * X - 1;

    /* TS 38.304 §7.1: PDCCH monitoring occasions for paging in the PF are sequentially numbered from zero
     * starting from the first such occasion. TS 38.213: MOs at offset + k*period; k0 = first in PF, k = current. */
    return nr_pcch_type2_po_mo_in_range(frame, slot, n_slots_frame, period, offset, start_mo, end_mo);
  }
}

/** @brief Schedule PCCH (paging) at the UE's paging occasion for the current (frame, slot).
 *
 * The network initiates paging by transmitting the Paging message at the UE's paging
 * occasion as specified in TS 38.304 §7.1 and TS 38.331 §5.3.2. This function pulls
 * the PCCH SDU and UE_ID from the MAC PCCH queue, checks whether the current (frame, slot) is a paging
 * occasion and, if so, schedules PDCCH/PDSCH with P‑RNTI for PCCH.
 *
 * PDCCH monitoring occasions and aggregation levels for paging follow TS 38.213 (Type0/Type2
 * PDCCH CSS for paging).
 *
 * The network may address multiple UEs in one Paging message (one PagingRecord per UE).
 * This function is called every TTI from the DL scheduler. It returns without scheduling
 * if (frameP, slotP) is not a paging occasion for the stored UE_ID or if there is no PCCH payload.
 *
 * @param mac     gNB MAC instance (common channels and RRC cell config)
 * @param frameP  Current SFN
 * @param slotP   Current slot
 * @param DL_req  DL TTI request to fill with PDCCH/PDSCH for P-RNTI
 * @param TX_req  TX data request for PDSCH payload */
void schedule_nr_pcch(gNB_MAC_INST *mac,
                      frame_t frameP,
                      slot_t slotP,
                      nfapi_nr_dl_tti_request_t *DL_req,
                      nfapi_nr_tx_data_request_t *TX_req)
{
  DevAssert(mac);
  DevAssert(DL_req);
  DevAssert(TX_req);

  const int CC_id = 0;
  NR_COMMON_channels_t *cc = &mac->common_channels[CC_id];
  NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;
  DevAssert(scc);
  DevAssert(scc->downlinkConfigCommon);

  /* PCCH-Config is in SIB1 (DownlinkConfigCommonSIB) */
  DevAssert(cc->sib1);
  DevAssert(cc->sib1->message.choice.c1);
  NR_SIB1_t *sib1 = cc->sib1->message.choice.c1->choice.systemInformationBlockType1;
  DevAssert(sib1);
  DevAssert(sib1->servingCellConfigCommon);
  const NR_PCCH_Config_t *pcch = &sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config;

  nr_mac_pcch_queue_t *q = &cc->pcch_queue;
  if (q->count == 0)
    return;
  const int head_idx = q->head;
  const nr_mac_pcch_item_t *pcch_pdu = &q->items[head_idx];
  const uint16_t pcch_ue_id = q->items[head_idx].ue_id;

  /* TS 38.304 §7.1: transmit Paging only at the UE's paging occasion (PF/PO),
   * checking the head element of the MAC PCCH queue. */
  if (!is_paging_occasion(pcch, frameP, slotP, pcch_ue_id, mac, scc))
    return;

  if (!is_dl_slot(slotP, &mac->frame_structure)) {
    LOG_W(NR_MAC,
          "[%04d.%02d][gNB %d] paging occasion in non-DL slot, skipping (UE_ID=%u)\n",
          frameP,
          slotP,
          mac->Mod_id,
          pcch_ue_id);
    return;
  }

  LOG_I(NR_MAC,
        "[%04d.%02d][gNB %d] Paging occasion for PCCH: UE_ID=%u pcch_pdu_len=%u\n",
        frameP,
        slotP,
        mac->Mod_id,
        pcch_ue_id,
        pcch_pdu->sdu_len);

  /* pagingSearchSpace (Type2-PDCCH CSS) */
  NR_SearchSpace_t *ss = get_paging_search_space(scc);
  DevAssert(ss != NULL);

  /* TS 38.213 §10.1: pagingSearchSpace (Type2-PDCCH CSS) references the controlResourceSetId of the CORESET
   * used for PDCCH paging monitoring occasions. */
  AssertFatal(ss->controlResourceSetId,
              "schedule_nr_pcch: paging search space id %ld has NULL controlResourceSetId\n",
              ss->searchSpaceId);
  NR_ControlResourceSet_t *coreset = get_coreset(mac, scc, NULL, *ss->controlResourceSetId);

  /* Beam selection for this paging occasion.
   * Spec (TS 38.304 §7.1, multi-beam): the same paging message and Short Message are repeated in all
   * transmitted beams. We currently transmit paging on a single SSB/beam (the first active SSB) per
   * occasion. TODO: extend to per-beam repetition. */
  int ssb_for_paging = 0;
  if (cc->num_active_ssb > 0)
    ssb_for_paging = cc->ssb_index[0];
  int beam_index = get_beam_from_ssbidx(mac, ssb_for_paging);
  const int n_slots_frame = mac->frame_structure.numb_slots_frame;
  NR_beam_alloc_t beam = beam_allocation_procedure(&mac->beam_info, frameP, slotP, beam_index, n_slots_frame);
  AssertFatal(beam.idx >= 0, "Cannot allocate beam for PCCH paging (ssb=%d)\n", ssb_for_paging);

  /* Determine CCE aggregation level and allocate CCE for PCCH PDCCH in the paging search space (Type2-PDCCH CSS set),
   * configured by pagingSearchSpace in PDCCH-ConfigCommon for DCI format 1_0 with CRC scrambled by P-RNTI
   * on the primary cell of the MCG. Aggregation levels and PDCCH candidates follow TS 38.213 §10.1 and Table 10.1-1;
   * PRBs used by this PDCCH are then reserved in the VRB map. */
  NR_BWP_t *initial_dl_bwp = &scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters;
  NR_sched_pdcch_t sched_pdcch = set_pdcch_structure(mac, ss, coreset, scc, initial_dl_bwp, mac->type0_PDCCH_CSS_config);
  int aggregation_level = 0;
  int CCEIndex = get_cce_index(mac, CC_id, slotP, 0, &aggregation_level, beam.idx, ss, coreset, &sched_pdcch, 0.0f);
  if (CCEIndex < 0) {
    LOG_W(NR_MAC, "[%04d.%02d][gNB %d] no free CCE for PCCH (UE_ID=%u)\n", frameP, slotP, mac->Mod_id, pcch_ue_id);
    return;
  }

  fill_pdcch_vrb_map(mac, CC_id, &sched_pdcch, CCEIndex, aggregation_level, beam.idx);

  /* Allocate PDSCH for PCCH and ensure TBS >= payload, using:
   * - TimeDomainAllocationList for common PDSCH on initial DL BWP
   * - TS 38.214 §5.1 (time-domain allocation) and §5.1.3.2 (TBS computation for PDSCH).
   * Try TDA entries in order and keep the first one whose RB/MCS/TBS allocation fits the payload. */
  NR_PDSCH_ConfigCommon_t *pdsch_cfg_common = scc->downlinkConfigCommon->initialDownlinkBWP->pdsch_ConfigCommon->choice.setup;
  NR_PDSCH_TimeDomainResourceAllocationList_t *tdalist = pdsch_cfg_common->pdsch_TimeDomainAllocationList;
  DevAssert(tdalist);
  AssertFatal(tdalist->list.count > 0, "PCCH-Config: pdsch-TimeDomainAllocationList is empty\n");

  uint16_t *vrb_map = cc->vrb_map[beam.idx];
  NR_sched_pdsch_t pdsch_pcch = {0};
  bool pdsch_ok = false;
  for (int t = 0; t < tdalist->list.count && !pdsch_ok; t++) {
    NR_tda_info_t tda_info = set_tda_info_from_list(tdalist, t);
    NR_pdsch_dmrs_t dmrs_parms = get_dl_dmrs_params(scc, NULL, &tda_info, 1);
    pdsch_pcch = (NR_sched_pdsch_t){
        .bwp_info = get_pdsch_bwp_start_size(mac, NULL),
        .time_domain_allocation = t,
        .dmrs_parms = dmrs_parms,
        .tda_info = tda_info,
        .nrOfLayers = 1,
        .pm_index = 0,
        .mcs = 0,
    };
    pdsch_ok = update_rb_mcs_tbs(&pdsch_pcch, pcch_pdu->sdu_len, vrb_map);
  }
  if (!pdsch_ok) {
    LOG_W(NR_MAC,
          "[%04d.%02d][gNB %d] cannot allocate PDSCH for PCCH after trying %d TDA entries (UE_ID=%u len=%u)\n",
          frameP,
          slotP,
          mac->Mod_id,
          tdalist->list.count,
          pcch_ue_id,
          pcch_pdu->sdu_len);
    return;
  }

  const uint16_t slbitmap = SL_to_bitmap(pdsch_pcch.tda_info.startSymbolIndex, pdsch_pcch.tda_info.nrOfSymbols);
  const int bwp_start = pdsch_pcch.bwp_info.bwpStart;
  for (int rb = 0; rb < pdsch_pcch.rbSize; rb++)
    vrb_map[rb + pdsch_pcch.rbStart + bwp_start] |= slbitmap;

  /* Build NFAPI DL_TTI.request and TX_DATA.request entries for PCCH:
   * - SCF/NFAPI DL_TTI PDCCH/PDSCH PDUs for P-RNTI (DCI 1_0)
   * - TX_data PDU carrying the PCCH payload, linked via PDU_index. */
  nfapi_nr_dl_tti_request_body_t *dl_req_body = &DL_req->dl_tti_request_body;
  int pdu_index = mac->pdu_index[CC_id]++;
  nr_fill_nfapi_dl_PCCH_pdu(mac,
                            &pdsch_pcch,
                            &sched_pdcch,
                            ss,
                            coreset,
                            aggregation_level,
                            CCEIndex,
                            dl_req_body,
                            pdu_index,
                            beam.idx);

  const int ntx_req = TX_req->Number_of_PDUs;
  nfapi_nr_pdu_t *tx_req = &TX_req->pdu_list[ntx_req];
  DevAssert(pdsch_pcch.tb_size >= pcch_pdu->sdu_len);
  DevAssert(pdsch_pcch.tb_size <= sizeof(tx_req->TLVs[0].value.direct));
  memset(tx_req->TLVs[0].value.direct, 0, pdsch_pcch.tb_size);
  memcpy(tx_req->TLVs[0].value.direct, pcch_pdu->sdu, pcch_pdu->sdu_len);
  tx_req->PDU_index = pdu_index;
  tx_req->num_TLV = 1;
  tx_req->TLVs[0].length = pdsch_pcch.tb_size;
  tx_req->PDU_length = compute_PDU_length(tx_req->num_TLV, tx_req->TLVs[0].length);
  TX_req->Number_of_PDUs++;
  TX_req->SFN = frameP;
  TX_req->Slot = slotP;

  pcch_queue_drop_head(q, head_idx);

  LOG_I(NR_MAC,
        "[%04d.%02d][gNB %d] PCCH: UE=%u ss=%ld cset=%ld beam=%d agg=%d CCE=%d tb=%d\n",
        frameP,
        slotP,
        mac->Mod_id,
        pcch_ue_id,
        ss->searchSpaceId,
        *ss->controlResourceSetId,
        beam.idx,
        aggregation_level,
        CCEIndex,
        pdsch_pcch.tb_size);
}
