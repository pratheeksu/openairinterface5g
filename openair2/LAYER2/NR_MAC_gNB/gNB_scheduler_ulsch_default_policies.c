/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Default pluggable UL scheduler policy functions
 *
 * These are the built-in defaults behind the UL scheduler function pointers.
 * Extracted from gNB_scheduler_ulsch.c to allow clean replacement by custom
 * scheduler plugins.
 */


#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "executables/softmodem-common.h"
#include "common/utils/nr/nr_common.h"
#include "utils.h"
#include <openair2/UTIL/OPT/opt.h>
#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"

/* Default UL RI/TPMI selector: reads rank and TPMI from SRS feedback.
 * A custom impl can compute joint (rank, TPMI) from the H matrix on the candidates. */
void nr_ul_ri_tpmi_select_default(gNB_MAC_INST *mac, nr_ul_candidate_t *cands, int n_cand)
{
  for (int i = 0; i < n_cand; i++) {
    NR_UE_sched_ctrl_t *sched_ctrl = &cands[i].UE->UE_sched_ctrl;
    NR_UE_UL_BWP_t *current_BWP = &cands[i].UE->current_UL_BWP;
    cands[i].alloc_nrOfLayers = (current_BWP->dci_format == NR_UL_DCI_FORMAT_0_0) ? 1 : sched_ctrl->srs_feedback.ul_ri + 1;
    cands[i].alloc_tpmi = sched_ctrl->srs_feedback.tpmi;
  }
}

/* Default UL TDA selector: picks the best TDA for the target slot (same for all cands).
 * Validates retx feasibility and drops infeasible retx cands (compact).
 * Each surviving cand gets alloc_tda/alloc_tda_info/alloc_slbitmap populated; retx cands get retx_rbSize set. */
int nr_ul_tda_select_default(gNB_MAC_INST *mac,
                             nr_ul_candidate_t *cands,
                             int n_cand,
                             frame_t sched_frame,
                             slot_t sched_slot,
                             int k2)
{
  const NR_tda_info_t *tda_list = NULL;
  int n_tda = get_num_ul_tda(mac, sched_slot, k2, &tda_list);
  if (n_tda == 0)
    return 0;

  NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  int bw = scc->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;
  int rb_start = 0, rb_len = bw;
  const NR_tda_info_t *best = get_best_ul_tda(mac, 0, tda_list, n_tda, sched_frame, sched_slot, &rb_start, &rb_len);
  DevAssert(best->valid_tda);
  int tda = seq_arr_dist(&mac->ul_tda, seq_arr_front(&mac->ul_tda), best);
  AssertFatal(tda >= 0 && tda < 16, "illegal TDA index %d\n", tda);
  uint16_t slbitmap = SL_to_bitmap(best->startSymbolIndex, best->nrOfSymbols);

  /* Assign TDA to all cands + validate retx */
  int n_valid = 0;
  for (int i = 0; i < n_cand; i++) {
    if (cands[i].skipped)
      continue;
    cands[i].alloc_tda = tda;
    cands[i].alloc_tda_info = *best;
    cands[i].alloc_slbitmap = slbitmap;

    if (cands[i].is_retx) {
      uint16_t needed = check_ul_retx_feasibility(&cands[i], tda, best, scc, cands[i].bwp_size);
      if (needed == 0) {
        LOG_D(NR_MAC, "[UE %04x] retx infeasible with TDA %d, deferring\n", cands[i].UE->rnti, tda);
        cands[i].skipped = true;
        continue;
      }
      cands[i].retx_rbSize = needed;
    }
    n_valid++;
  }
  return n_valid;
}

int nr_ul_beam_select_default(NR_beam_info_t *beam_info,
                              const int16_t *beam_index_list,
                              nr_ul_candidate_t *candidates,
                              int n_candidates,
                              frame_t frame,
                              slot_t slot,
                              frame_t sched_frame,
                              slot_t sched_slot,
                              int slots_per_frame)
{
  /* Iterate in PF priority order so retx and high-priority UEs claim beams first.
   * Inline rank check: O(1) per iteration, sorts the tail only if a custom
   * upstream stage broke PF rank order. */
  int n_valid = 0;
  int last_rank = -1;
  for (int i = 0; i < n_candidates; i++) {
    nr_ul_candidate_t *cand = &candidates[i];
    if (cand->skipped)
      continue;
    if (cand->pf_rank < last_rank)
      qsort(candidates + i, n_candidates - i, sizeof(*candidates), compare_ul_pf_rank);
    last_rank = cand->pf_rank;

    /* Allocate beam for DCI slot */
    NR_beam_alloc_t dci_beam = beam_allocation_procedure(beam_info, frame, slot, cand->beam_index, slots_per_frame);
    if (dci_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] DCI beam could not be allocated\n", cand->UE->rnti, frame, slot);
      cand->skipped = true;
      continue;
    }

    /* Allocate beam for scheduled PUSCH slot */
    NR_beam_alloc_t sched_beam = beam_allocation_procedure(beam_info, sched_frame, sched_slot, cand->beam_index, slots_per_frame);
    if (sched_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Sched beam could not be allocated\n", cand->UE->rnti, frame, slot);
      reset_beam_status(beam_info, frame, slot, cand->beam_index, slots_per_frame, dci_beam.new_beam);
      cand->skipped = true;
      continue;
    }

    cand->alloc_dci_beam_idx = dci_beam.idx;
    cand->alloc_dci_beam_new = dci_beam.new_beam;
    cand->alloc_beam_idx = sched_beam.idx;
    cand->alloc_sched_beam_new = sched_beam.new_beam;
    n_valid++;
  }
  return n_valid;
}

void nr_ul_mcs_select_default(const gNB_MAC_INST *mac, nr_ul_candidate_t *candidates, int n_candidates)
{
  const NR_bler_options_t *bo = &mac->ul_bler;
  for (int i = 0; i < n_candidates; i++) {
    nr_ul_candidate_t *cand = &candidates[i];
    int mcs;
    if (cand->is_retx) {
      mcs = cand->current_mcs;
    } else if (bo->harq_round_max == 1) {
      mcs = get_mcs_from_SINRx10(cand->mcs_table, cand->snrx10, cand->alloc_nrOfLayers);
      mcs = max(bo->min_mcs, min(bo->max_mcs, min(cand->max_mcs, mcs)));
    } else if (!cand->bler_updated) {
      mcs = cand->current_mcs;
    } else {
      mcs = nr_adapt_mcs_from_bler(cand->current_mcs,
                                   bo->min_mcs,
                                   cand->max_mcs,
                                   cand->bler,
                                   bo->lower,
                                   bo->upper,
                                   cand->last_num_sched);
    }
    cand->alloc_mcs = mcs;
    if (!cand->is_retx)
      cand->UE->UE_sched_ctrl.ul_bler_stats.mcs = mcs;
  }
}

int nr_ul_proportional_fair(const nr_ul_sched_params_t *params, nr_ul_candidate_t *candidates, int *sched_indices, int n_candidates)
{
  int n_scheduled = 0;
  const int min_rb = params->min_rb;

  /* Per-beam scan: iterate all candidates for each beam. Array order = PF
   * priority order, so within each beam we process retx first, then
   * highest-priority new-tx first. No grouping sort needed.
   * Phase 1 includes an inline rank check: O(1) per iteration, sorts the
   * tail only if a custom upstream stage broke PF rank order. Once healed
   * in phase 1, phases 2-3 see a sorted array. */
  int last_rank = -1;
  for (int b = 0; b < params->num_beams; b++) {
    uint16_t *vrb_map = params->vrb_map_UL[b];

    /* Phase 1: HARQ retransmissions (highest priority, exact RBs) */
    for (int i = 0; i < n_candidates; i++) {
      if (candidates[i].skipped || candidates[i].alloc_beam_idx != b || !candidates[i].is_retx)
        continue;
      if (candidates[i].pf_rank < last_rank)
        qsort(candidates + i, n_candidates - i, sizeof(*candidates), compare_ul_pf_rank);
      last_rank = candidates[i].pf_rank;

      int rbStart;
      int block_len = find_largest_free_block(vrb_map, candidates[i].alloc_slbitmap, candidates[i].bwp_start, candidates[i].bwp_size, &rbStart);
      if (block_len < candidates[i].retx_rbSize)
        continue;

      COMMIT_UL_ALLOC(params, i, &candidates[i], rbStart, candidates[i].retx_rbSize, candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 2: Inactive UEs (no BSR data, need scheduling for TA/SR) */
    for (int i = 0; i < n_candidates; i++) {
      if (candidates[i].skipped || candidates[i].alloc_beam_idx != b)
        continue;
      if (candidates[i].is_retx || !candidates[i].sched_inactive)
        continue;

      int rbStart;
      int block_len = find_largest_free_block(vrb_map, candidates[i].alloc_slbitmap, candidates[i].bwp_start, candidates[i].bwp_size, &rbStart);
      if (block_len < min_rb)
        continue;

      COMMIT_UL_ALLOC(params, i, &candidates[i], rbStart, min_rb, candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 3: New data UEs — array order is PF priority, largest free block */
    for (int i = 0; i < n_candidates; i++) {
      nr_ul_candidate_t *cand = &candidates[i];
      if (cand->skipped || cand->alloc_beam_idx != b)
        continue;
      if (cand->is_retx || cand->sched_inactive)
        continue;

      int block_start;
      int block_len = find_largest_free_block(vrb_map, cand->alloc_slbitmap, cand->bwp_start, cand->bwp_size, &block_start);
      if (block_len < min_rb)
        continue;

      uint16_t rbSize = block_len;
      uint8_t mcs = cand->alloc_mcs;

      if (cand->pcmax != 0 || cand->ph != 0) {
        nr_ul_phr_advice_t advice;
        if (!nr_ul_check_phr(params, cand, rbSize, mcs, &advice)) {
          rbSize = advice.max_mcs_min_rb.rbSize;
          mcs = advice.max_mcs_min_rb.mcs;
        }
      }

      NR_UE_UL_BWP_t *current_BWP = &cand->UE->current_UL_BWP;
      NR_pusch_dmrs_t dmrs_info = get_ul_dmrs_params(params->scc, current_BWP, &cand->alloc_tda_info, cand->alloc_nrOfLayers);
      uint16_t Rt;
      uint8_t Qt;
      update_ul_ue_R_Qm(mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &Rt, &Qt);
      uint32_t tb_size;
      uint16_t final_rbSize;
      nr_find_nb_rb(Qt,
                    Rt,
                    current_BWP->transform_precoding,
                    cand->alloc_nrOfLayers,
                    cand->alloc_tda_info.nrOfSymbols,
                    dmrs_info.N_PRB_DMRS * dmrs_info.num_dmrs_symb,
                    cand->pending_bytes,
                    min_rb,
                    rbSize,
                    &tb_size,
                    &final_rbSize);

      COMMIT_UL_ALLOC(params, i, cand, block_start, final_rbSize, mcs, sched_indices, n_scheduled);
    }
  }

  return n_scheduled;
}
