/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief       Default pluggable policy functions for the DL scheduler pipeline.
 *
 * These are the built-in implementations behind the function pointers
 * (dl_tda_select, dl_beam_select, dl_mcs_select, dl_rb_alloc, dl_lcid_alloc)
 * wired up at MAC init time.  They can be replaced at runtime by external
 * scheduler plug-ins without touching the core scheduling loop in
 * gNB_scheduler_dlsch.c.
 */

#include "common/utils/nr/nr_common.h"
/*MAC*/
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"

/*TAG*/
#include "NR_TAG-Id.h"

/*Softmodem params*/
#include "executables/softmodem-common.h"
#include "../../../nfapi/oai_integration/vendor_ext.h"

// Default TDA selector: picks the slot-wide TDA index from get_dl_tda(),
// then resolves tda_info per candidate using each UE's own BWP / search
// space / coreset. Marks invalids with skipped=true.
int nr_dl_tda_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates, frame_t frame, slot_t slot)
{
  int tda = get_dl_tda(mac, slot);
  AssertFatal(tda >= 0, "Unable to find PDSCH time domain allocation in list\n");
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;

  int n_valid = 0;
  for (int i = 0; i < n_candidates; i++) {
    if (candidates[i].skipped)
      continue;
    NR_UE_info_t *UE = candidates[i].UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
    int coresetid = sched_ctrl->coreset->controlResourceSetId;
    NR_tda_info_t tda_info = get_dl_tda_info(dl_bwp,
                                             sched_ctrl->search_space->searchSpaceType->present,
                                             tda,
                                             scc->dmrs_TypeA_Position,
                                             1,
                                             TYPE_C_RNTI_,
                                             coresetid,
                                             false);
    if (!tda_info.valid_tda) {
      candidates[i].skipped = true;
      continue;
    }

    candidates[i].alloc_tda = tda;
    candidates[i].alloc_tda_info = tda_info;
    candidates[i].alloc_slbitmap = SL_to_bitmap(tda_info.startSymbolIndex, tda_info.nrOfSymbols);
    n_valid++;
  }
  return n_valid;
}

int nr_dl_beam_select_default(NR_beam_info_t *beam_info,
                              const int16_t *beam_index_list,
                              nr_dl_candidate_t *candidates,
                              int n_candidates,
                              frame_t frame,
                              slot_t slot,
                              int slots_per_frame)
{
  /* Iterate in PF priority order so retx and high-priority UEs claim beams first.
   * Inline rank check: O(1) per iteration, sorts the tail only if a custom
   * upstream stage broke PF rank order. */
  int n_valid = 0;
  int last_rank = -1;
  for (int i = 0; i < n_candidates; i++) {
    if (candidates[i].skipped)
      continue;
    if (candidates[i].pf_rank < last_rank)
      qsort(candidates + i, n_candidates - i, sizeof(*candidates), compare_dl_pf_rank);
    last_rank = candidates[i].pf_rank;

    NR_beam_alloc_t beam = beam_allocation_procedure(beam_info, frame, slot, candidates[i].alloc_beam_dir, slots_per_frame);
    if (beam.idx < 0) {
      candidates[i].skipped = true;
      continue;
    }

    candidates[i].alloc_beam_idx = beam.idx;
    candidates[i].alloc_new_beam = beam.new_beam;
    n_valid++;
  }
  return n_valid;
}

void nr_dl_mcs_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates)
{
  const NR_bler_options_t *bo = &mac->dl_bler;
  for (int i = 0; i < n_candidates; i++) {
    nr_dl_candidate_t *cand = &candidates[i];
    int mcs;
    if (cand->is_retx) {
      mcs = cand->current_mcs; /* retx MCS is fixed by the HARQ round */
    } else if (bo->harq_round_max == 1) {
      mcs = max(bo->min_mcs, min(bo->max_mcs, cand->max_mcs));
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
    /* Persist for all candidates — BLER-based MCS ramps even for UEs the
     * policy rejects this slot (failed CCE, no free RBs, etc.). */
    if (!cand->is_retx)
      cand->UE->UE_sched_ctrl.dl_bler_stats.mcs = mcs;
  }
}

int nr_dl_proportional_fair(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int *sched_indices, int n_candidates)
{
  const int min_rbSize = 5;
  int n_scheduled = 0;

  /* Per-beam scan: iterate all candidates for each beam. Array order = PF
   * priority order, so within each beam we process retx first, then
   * highest-priority new-tx first. No grouping sort needed.
   * Phase 1 includes an inline rank check: O(1) per iteration, sorts the
   * tail only if a custom upstream stage broke PF rank order. Once healed
   * in phase 1, phases 2-3 see a sorted array. */
  int last_rank = -1;
  for (int b = 0; b < params->num_beams; b++) {
    uint16_t *vrb_map = params->vrb_map[b];

    /* Phase 1: HARQ retransmissions (highest priority, exact RBs) */
    for (int i = 0; i < n_candidates; i++) {
      if (candidates[i].skipped || candidates[i].alloc_beam_idx != b || !candidates[i].is_retx)
        continue;
      if (candidates[i].pf_rank < last_rank)
        qsort(candidates + i, n_candidates - i, sizeof(*candidates), compare_dl_pf_rank);
      last_rank = candidates[i].pf_rank;

      int needed_rbs = candidates[i].retx_rbSize;
      int rbStart, rbSize;
      if (!get_rb_alloc(needed_rbs,
                        candidates[i].bwp_size,
                        candidates[i].bwp_start,
                        candidates[i].bwp_size,
                        vrb_map,
                        candidates[i].alloc_slbitmap,
                        &rbStart,
                        &rbSize))
        continue;

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, needed_rbs, candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 2: No-data UEs (TA command or beam switch MAC CE, no RLC data) */
    for (int i = 0; i < n_candidates; i++) {
      if (candidates[i].skipped || candidates[i].alloc_beam_idx != b)
        continue;
      if (candidates[i].is_retx || candidates[i].pending_bytes > 0)
        continue;

      int rbStart, rbSize;
      if (!get_rb_alloc(min_rbSize,
                        candidates[i].bwp_size,
                        candidates[i].bwp_start,
                        candidates[i].bwp_size,
                        vrb_map,
                        candidates[i].alloc_slbitmap,
                        &rbStart,
                        &rbSize))
        continue;

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, min_rbSize, candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 3: New data UEs — array order is PF priority, largest free block */
    for (int i = 0; i < n_candidates; i++) {
      if (candidates[i].skipped || candidates[i].alloc_beam_idx != b)
        continue;
      if (candidates[i].is_retx || candidates[i].pending_bytes == 0)
        continue;

      int rbStart;
      int max_rbSize =
          find_largest_free_block(vrb_map, candidates[i].alloc_slbitmap, candidates[i].bwp_start, candidates[i].bwp_size, &rbStart);
      if (max_rbSize < min_rbSize)
        continue;

      int mcs = candidates[i].alloc_mcs;
      uint8_t Qm = nr_get_Qm_dl(mcs, candidates[i].mcs_table);
      uint16_t R = nr_get_code_rate_dl(mcs, candidates[i].mcs_table);
      NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(params->mac->common_channels->ServingCellConfigCommon,
                                                &candidates[i].UE->current_DL_BWP,
                                                &candidates[i].alloc_tda_info,
                                                candidates[i].alloc_nrOfLayers);
      const int oh = 3 * 4 + (candidates[i].UE->UE_sched_ctrl.ta_apply ? 2 : 0);
      uint32_t tbs;
      uint16_t rbSize;
      nr_find_nb_rb(Qm,
                    R,
                    1,
                    candidates[i].alloc_nrOfLayers,
                    candidates[i].alloc_tda_info.nrOfSymbols,
                    dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                    candidates[i].pending_bytes + oh,
                    min_rbSize,
                    max_rbSize,
                    &tbs,
                    &rbSize);

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, rbSize, mcs, sched_indices, n_scheduled);
    }
  }

  return n_scheduled;
}

void nr_dl_lcid_alloc_default(const gNB_MAC_INST *mac,
                                     const nr_dl_candidate_t *candidate,
                                     int tbs_available,
                                     int lcid_alloc[NR_MAX_NUM_LCID])
{
  (void)mac;
  (void)tbs_available;
  memset(lcid_alloc, 0, NR_MAX_NUM_LCID * sizeof(int));
  for (int lcid = 0; lcid < NR_MAX_NUM_LCID; lcid++)
    lcid_alloc[lcid] = candidate->pending_bytes_per_lcid[lcid];
}
