/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief       procedures related to gNB for the DLSCH transport channel
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

////////////////////////////////////////////////////////
/////* DLSCH MAC PDU generation (6.1.2 TS 38.321) */////
////////////////////////////////////////////////////////
#define OCTET 8
#define WORD 32
//#define SIZE_OF_POINTER sizeof (void *)

#define MAX_NUM_DATA_REQ 1024

int get_dl_tda(const gNB_MAC_INST *nrmac, int slot)
{
  /* we assume that this function is mutex-protected from outside */
  const frame_structure_t *fs = &nrmac->frame_structure;

  // Use special TDA in case of CSI-RS
  if (nrmac->UE_info.sched_csirs > 0)
    return 1;

  if (fs->frame_type == TDD) {
    int s = get_slot_idx_in_period(slot, fs);
    // if there is a mixed slot where we can transmit DL
    const tdd_bitmap_t *tdd_slot_bitmap = fs->period_cfg.tdd_slot_bitmap;
    if (tdd_slot_bitmap[s].num_dl_symbols > 1 && is_mixed_slot(s, fs)) {
      return 2;
    }
  }
  return 0; // if FDD or not mixed slot in TDD, for now use default TDA
}

// Compute and write all MAC CEs and subheaders, and return number of written bytes
int nr_write_ce_dlsch_pdu(module_id_t module_idP,
                          const NR_UE_sched_ctrl_t *ue_sched_ctl,
                          unsigned char *mac_pdu,
                          unsigned char drx_cmd,
                          unsigned char *ue_cont_res_id)
{
  gNB_MAC_INST *gNB = RC.nrmac[module_idP];
  /* already mutex protected: called below and in _RA.c */
  NR_SCHED_ENSURE_LOCKED(&gNB->sched_lock);

  NR_MAC_SUBHEADER_FIXED *mac_pdu_ptr = (NR_MAC_SUBHEADER_FIXED *) mac_pdu;
  uint8_t last_size = 0;
  int offset = 0, mac_ce_size, i, timing_advance_cmd, tag_id = 0;
  // MAC CEs
  uint8_t mac_header_control_elements[16], *ce_ptr;
  ce_ptr = &mac_header_control_elements[0];

  // DRX command subheader (MAC CE size 0)
  if (drx_cmd != 255) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_DRX;
    //last_size = 1;
    mac_pdu_ptr++;
  }

  // Timing Advance subheader
  /* This was done only when timing_advance_cmd != 31
  // now TA is always send when ta_timer resets regardless of its value
  // this is done to avoid issues with the timeAlignmentTimer which is
  // supposed to monitor if the UE received TA or not */
  if (ue_sched_ctl->ta_apply) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_TA_COMMAND;
    //last_size = 1;
    mac_pdu_ptr++;
    // TA MAC CE (1 octet)
    timing_advance_cmd = ue_sched_ctl->ta_update;
    AssertFatal(timing_advance_cmd < 64, "timing_advance_cmd %d > 63\n", timing_advance_cmd);
    ((NR_MAC_CE_TA *) ce_ptr)->TA_COMMAND = timing_advance_cmd;    //(timing_advance_cmd+31)&0x3f;

    tag_id = gNB->tag->tag_Id;
    ((NR_MAC_CE_TA *) ce_ptr)->TAGID = tag_id;

    LOG_D(NR_MAC, "NR MAC CE timing advance command = %d (%d) TAG ID = %d\n", timing_advance_cmd, ((NR_MAC_CE_TA *) ce_ptr)->TA_COMMAND, tag_id);
    mac_ce_size = sizeof(NR_MAC_CE_TA);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *) ce_ptr, mac_ce_size);
    ce_ptr += mac_ce_size;
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  // Contention resolution fixed subheader and MAC CE
  if (ue_cont_res_id) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_CON_RES_ID;
    mac_pdu_ptr++;
    //last_size = 1;
    // contention resolution identity MAC ce has a fixed 48 bit size
    // this contains the UL CCCH SDU. If UL CCCH SDU is longer than 48 bits,
    // it contains the first 48 bits of the UL CCCH SDU
    LOG_D(NR_MAC,
          "[gNB ][RAPROC] Generate contention resolution msg: %x.%x.%x.%x.%x.%x\n",
          ue_cont_res_id[0],
          ue_cont_res_id[1],
          ue_cont_res_id[2],
          ue_cont_res_id[3],
          ue_cont_res_id[4],
          ue_cont_res_id[5]);
    // Copying bytes (6 octects) to CEs pointer
    mac_ce_size = 6;
    memcpy(ce_ptr, ue_cont_res_id, mac_ce_size);
    // Copying bytes for MAC CEs to mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *) ce_ptr, mac_ce_size);
    ce_ptr += mac_ce_size;
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.15 TCI State indication for UE Specific PDCCH MAC CE SubPDU generation
  if (ue_sched_ctl->UE_mac_ce_ctrl.tci_state_ind.is_scheduled) {
    //filling subheader
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_TCI_STATE_IND_UE_SPEC_PDCCH;
    mac_pdu_ptr++;
    //Creating the instance of CE structure
    const tciStateInd_t *tcisi = &ue_sched_ctl->UE_mac_ce_ctrl.tci_state_ind;
    NR_TCI_PDCCH  nr_UESpec_TCI_StateInd_PDCCH = {
      .CoresetId1 = (tcisi->coresetId & 0xF) >> 1,
      .ServingCellId = 0, // TODO this is likely ServingCellIndex, see 38.331
      .TciStateId = tcisi->tciStateId & 0x7F,
      .CoresetId2 = tcisi->coresetId & 0x1,
    };
    LOG_I(NR_MAC, "NR MAC CE TCI state indication for UE Specific PDCCH = %d \n", nr_UESpec_TCI_StateInd_PDCCH.TciStateId);
    mac_ce_size = sizeof(NR_TCI_PDCCH);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)&nr_UESpec_TCI_StateInd_PDCCH, mac_ce_size);
    //incrementing the PDU pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.16, SP CSI reporting on PUCCH Activation/Deactivation MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.is_scheduled) {
    //filling the subheader
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_SP_CSI_REP_PUCCH_ACT;
    mac_pdu_ptr++;
    //creating the instance of CE structure
    NR_PUCCH_CSI_REPORTING nr_PUCCH_CSI_reportingActDeact;
    //filling the CE structure
    nr_PUCCH_CSI_reportingActDeact.BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.bwpId) & 0x3; //extracting LSB 2 bibs
    nr_PUCCH_CSI_reportingActDeact.ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.servingCellId) & 0x1F; //extracting LSB 5 bits
    nr_PUCCH_CSI_reportingActDeact.S0 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[0];
    nr_PUCCH_CSI_reportingActDeact.S1 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[1];
    nr_PUCCH_CSI_reportingActDeact.S2 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[2];
    nr_PUCCH_CSI_reportingActDeact.S3 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[3];
    nr_PUCCH_CSI_reportingActDeact.R2 = 0;
    mac_ce_size = sizeof(NR_PUCCH_CSI_REPORTING);
    // Copying MAC CE data to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)&nr_PUCCH_CSI_reportingActDeact, mac_ce_size);
    //incrementing the PDU pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.14, TCI State activation/deactivation for UE Specific PDSCH MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.is_scheduled) {
    //Computing the number of octects to be allocated for Flexible array member
    //of MAC CE structure
    uint8_t num_octects = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.highestTciStateActivated) / 8 + 1; //Calculating the number of octects for allocating the memory
    //filling the subheader
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->F = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->LCID = DL_SCH_LCID_TCI_STATE_ACT_UE_SPEC_PDSCH;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->L = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    last_size = 2;
    //Incrementing the PDU pointer
    mac_pdu_ptr += last_size;
    //allocating memory for CE Structure
    NR_TCI_PDSCH_APERIODIC_CSI *nr_UESpec_TCI_StateInd_PDSCH = (NR_TCI_PDSCH_APERIODIC_CSI *)malloc(sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //initializing to zero
    memset((void *)nr_UESpec_TCI_StateInd_PDSCH, 0, sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //filling the CE Structure
    nr_UESpec_TCI_StateInd_PDSCH->BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.bwpId) & 0x3; //extracting LSB 2 Bits
    nr_UESpec_TCI_StateInd_PDSCH->ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.servingCellId) & 0x1F; //extracting LSB 5 bits

    for(i = 0; i < (num_octects * 8); i++) {
      if(ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.tciStateActDeact[i])
        nr_UESpec_TCI_StateInd_PDSCH->T[i / 8] = nr_UESpec_TCI_StateInd_PDSCH->T[i / 8] | (1 << (i % 8));
    }

    mac_ce_size = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    //Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)nr_UESpec_TCI_StateInd_PDSCH, mac_ce_size);
    //incrementing the mac pdu pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
    //freeing the allocated memory
    free(nr_UESpec_TCI_StateInd_PDSCH);
  }

  //TS38.321 Sec 6.1.3.13 Aperiodic CSI Trigger State Subselection MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.is_scheduled) {
    //Computing the number of octects to be allocated for Flexible array member
    //of MAC CE structure
    uint8_t num_octects = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.highestTriggerStateSelected) / 8 + 1; //Calculating the number of octects for allocating the memory
    //filling the subheader
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->F = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->LCID = DL_SCH_LCID_APERIODIC_CSI_TRI_STATE_SUBSEL;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->L = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    last_size = 2;
    //Incrementing the PDU pointer
    mac_pdu_ptr += last_size;
    //allocating memory for CE structure
    NR_TCI_PDSCH_APERIODIC_CSI *nr_Aperiodic_CSI_Trigger = (NR_TCI_PDSCH_APERIODIC_CSI *)malloc(sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //initializing to zero
    memset((void *)nr_Aperiodic_CSI_Trigger, 0, sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //filling the CE Structure
    nr_Aperiodic_CSI_Trigger->BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.bwpId) & 0x3; //extracting LSB 2 bits
    nr_Aperiodic_CSI_Trigger->ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.servingCellId) & 0x1F; //extracting LSB 5 bits
    nr_Aperiodic_CSI_Trigger->R = 0;

    for(i = 0; i < (num_octects * 8); i++) {
      if(ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.triggerStateSelection[i])
        nr_Aperiodic_CSI_Trigger->T[i / 8] = nr_Aperiodic_CSI_Trigger->T[i / 8] | (1 << (i % 8));
    }

    mac_ce_size = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)nr_Aperiodic_CSI_Trigger, mac_ce_size);
    //incrementing the mac pdu pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
    //freeing the allocated memory
    free(nr_Aperiodic_CSI_Trigger);
  }

  if (ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.is_scheduled) {
    ((NR_MAC_SUBHEADER_FIXED *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_FIXED *) mac_pdu_ptr)->LCID = DL_SCH_LCID_SP_ZP_CSI_RS_RES_SET_ACT;
    mac_pdu_ptr++;
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->A_D = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.act_deact;
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->CELLID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.serv_cell_id & 0x1F; //5 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->BWPID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.bwpid & 0x3; //2 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->CSIRS_RSC_ID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.rsc_id & 0xF; //4 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->R = 0;
    LOG_D(NR_MAC, "NR MAC CE of ZP CSIRS Serv cell ID = %d BWPID= %d Rsc set ID = %d\n", ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.serv_cell_id, ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.bwpid,
          ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.rsc_id);
    mac_ce_size = sizeof(NR_MAC_CE_SP_ZP_CSI_RS_RES_SET);
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  if (ue_sched_ctl->UE_mac_ce_ctrl.csi_im.is_scheduled) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_SP_CSI_RS_CSI_IM_RES_SET_ACT;
    mac_pdu_ptr++;
    CSI_RS_CSI_IM_ACT_DEACT_MAC_CE csi_rs_im_act_deact_ce;
    csi_rs_im_act_deact_ce.A_D = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.act_deact;
    csi_rs_im_act_deact_ce.SCID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.serv_cellid & 0x3F;//gNB_PHY -> ssb_pdu.ssb_pdu_rel15.PhysCellId;
    csi_rs_im_act_deact_ce.BWP_ID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.bwp_id;
    csi_rs_im_act_deact_ce.R1 = 0;
    csi_rs_im_act_deact_ce.IM = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.im;// IF set CSI IM Rsc id will presesent else CSI IM RSC ID is abscent
    csi_rs_im_act_deact_ce.SP_CSI_RSID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.nzp_csi_rsc_id;

    if ( csi_rs_im_act_deact_ce.IM ) { //is_scheduled if IM is 1 else this field will not present
      csi_rs_im_act_deact_ce.R2 = 0;
      csi_rs_im_act_deact_ce.SP_CSI_IMID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.csi_im_rsc_id;
      mac_ce_size = sizeof ( csi_rs_im_act_deact_ce ) - sizeof ( csi_rs_im_act_deact_ce.TCI_STATE );
    } else {
      mac_ce_size = sizeof ( csi_rs_im_act_deact_ce ) - sizeof ( csi_rs_im_act_deact_ce.TCI_STATE ) - 1;
    }

    memcpy ((void *) mac_pdu_ptr, (void *) & ( csi_rs_im_act_deact_ce), mac_ce_size);
    mac_pdu_ptr += (unsigned char) mac_ce_size;

    if (csi_rs_im_act_deact_ce.A_D ) { //Following IE is_scheduled only if A/D is 1
      mac_ce_size = sizeof ( struct TCI_S);

      for ( i = 0; i < ue_sched_ctl->UE_mac_ce_ctrl.csi_im.nb_tci_resource_set_id; i++) {
        csi_rs_im_act_deact_ce.TCI_STATE.R = 0;
        csi_rs_im_act_deact_ce.TCI_STATE.TCI_STATE_ID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.tci_state_id [i] & 0x7F;
        memcpy ((void *) mac_pdu_ptr, (void *) & (csi_rs_im_act_deact_ce.TCI_STATE), mac_ce_size);
        mac_pdu_ptr += (unsigned char) mac_ce_size;
      }
    }
  }

  // compute final offset
  offset = ((unsigned char *) mac_pdu_ptr - mac_pdu);
  //printf("Offset %d \n", ((unsigned char *) mac_pdu_ptr - mac_pdu));
  return offset;
}

static uint32_t update_dlsch_buffer(frame_t frame, slot_t slot, NR_UE_info_t *UE)
{
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  sched_ctrl->num_total_bytes = 0;
  int dl_pdus_total = 0;

  logical_chan_id_t ch[NR_MAX_NUM_LCID] = {0};
  int n = 0;
  /* loop over all activated logical channels */
  FOR_EACH_SEQ_ARR(const nr_lc_config_t *, c, &sched_ctrl->lc_config ) {
    logical_chan_id_t lcid = c->lcid;
    if (c->suspended || (lcid == DL_SCH_LCID_DTCH && nr_timer_is_active(&sched_ctrl->transm_interrupt))) {
      memset(&sched_ctrl->rlc_status[lcid], 0, sizeof(sched_ctrl->rlc_status[lcid]));
      continue;
    }
    ch[n++] = lcid;
  }

  mac_rlc_status_resp_t ret[NR_MAX_NUM_LCID] = {0};
  nr_mac_rlc_status_ind(UE->rnti, frame, n, ch, ret);

  for (int i = 0; i < n; ++i) {
    logical_chan_id_t lcid = ch[i];

    if (ret[i].bytes_in_buffer == 0)
      continue;

    sched_ctrl->rlc_status[lcid] = ret[i];
    dl_pdus_total += sched_ctrl->rlc_status[lcid].pdus_in_buffer;
    sched_ctrl->num_total_bytes += sched_ctrl->rlc_status[lcid].bytes_in_buffer;
    LOG_D(MAC,
          "%4d.%2d UE %04x LCID %d status: %d bytes, %d PDUs, total buffer %d bytes %d PDUs\n",
          frame,
          slot,
          UE->rnti,
          lcid,
          ret[i].bytes_in_buffer,
          ret[i].pdus_in_buffer,
          sched_ctrl->num_total_bytes,
          dl_pdus_total);
  }
  return sched_ctrl->num_total_bytes;
}

void finish_nr_dl_harq(NR_UE_sched_ctrl_t *sched_ctrl, int harq_pid)
{
  NR_UE_harq_t *harq = &sched_ctrl->harq_processes[harq_pid];

  harq->ndi ^= 1;
  harq->round = 0;

  add_tail_nr_list(&sched_ctrl->available_dl_harq, harq_pid);
}

void abort_nr_dl_harq(NR_UE_info_t* UE, int8_t harq_pid)
{
  /* already mutex protected through handle_dl_harq() */
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

  finish_nr_dl_harq(sched_ctrl, harq_pid);
  UE->mac_stats.dl.errors++;
}

bwp_info_t get_pdsch_bwp_start_size(gNB_MAC_INST *nr_mac, NR_UE_info_t *UE)
{
  bwp_info_t bwp_info;
  if (!UE) {
    bwp_info.bwpStart = nr_mac->cset0_bwp_start;
    bwp_info.bwpSize = nr_mac->cset0_bwp_size;
    return bwp_info;
  }
  NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  // 3GPP TS 38.214 Section 5.1.2.2 Resource allocation in frequency domain
  // For a PDSCH scheduled with a DCI format 1_0 in any type of PDCCH common search space, regardless of which bandwidth part is the
  // active bandwidth part, RB numbering starts from the lowest RB of the CORESET in which the DCI was received; otherwise RB
  // numbering starts from the lowest RB in the determined downlink bandwidth part.
  //
  // 3GPP TS 38.214 Section 5.1.2.2.2 Downlink resource allocation type 1
  // In downlink resource allocation of type 1, the resource block assignment information indicates to a scheduled UE a set of
  // contiguously allocated non-interleaved or interleaved virtual resource blocks within the active bandwidth part of size   PRBs
  // except for the case when DCI format 1_0 is decoded in any common search space in which case the size of CORESET 0 shall be
  // used if CORESET 0 is configured for the cell and the size of initial DL bandwidth part shall be used if CORESET 0 is not
  // configured for the cell.
  if (dl_bwp->dci_format == NR_DL_DCI_FORMAT_1_0
      && sched_ctrl->search_space->searchSpaceType
      && sched_ctrl->search_space->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_common) {
    if (sched_ctrl->coreset->controlResourceSetId == 0) {
      bwp_info.bwpStart = nr_mac->cset0_bwp_start;
    } else {
      int additional_offset = (dl_bwp->BWPStart + 5) / 6 * 6 - dl_bwp->BWPStart;
      bwp_info.bwpStart = dl_bwp->BWPStart + sched_ctrl->sched_pdcch.rb_start + additional_offset;
    }
    if (nr_mac->cset0_bwp_size > 0) {
      bwp_info.bwpSize = min(dl_bwp->BWPSize, nr_mac->cset0_bwp_size);
    } else {
      bwp_info.bwpSize = min(dl_bwp->BWPSize, UE->sc_info.initial_dl_BWPSize);
    }
  } else {
    bwp_info.bwpSize = dl_bwp->BWPSize;
    bwp_info.bwpStart = dl_bwp->BWPStart;
  }
  return bwp_info;
}

static void ack_reconfig(gNB_MAC_INST *mac, NR_UE_info_t *UE)
{
  if (!UE->reconfigCellGroup) {
    LOG_W(NR_MAC, "Received ACK for RRCReconfiguration, but nothing to apply!\n");
    return;
  }
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, UE->CellGroup);
  UE->CellGroup = UE->reconfigCellGroup;
  UE->reconfigCellGroup = NULL;
  NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  /* clean BWP structures */
  clean_bwp_structures(UE->CellGroup->spCellConfig);
  configure_UE_BWP(mac, scc, UE, false, NR_SearchSpace__searchSpaceType_PR_common, -1, -1);
}

static bool dlsch_to_schedule(const NR_UE_sched_ctrl_t *sched_ctrl)
{
  /* Check DL buffer, TA to be sent and  beam switch needed*/
  if (sched_ctrl->num_total_bytes > 0)
    return true;
  if (sched_ctrl->ta_apply == true)
    return true;
  if (sched_ctrl->UE_mac_ce_ctrl.tci_state_ind.is_scheduled)
    return true;
  // if none of the condition for dlsch to be scheduled are met
  return false;
}


// Find the largest contiguous block of free RBs in VRB map.
// Returns true if any free RBs found, with rbStart and rbSize set.
static int collect_dl_candidates(gNB_MAC_INST *mac,
                                 NR_UE_info_t **UE_list,
                                 nr_dl_candidate_t *candidates,
                          int max_candidates,
                          frame_t frame,
                          slot_t slot)
{
  int n = 0;
  const frame_structure_t *fs = &mac->frame_structure;
  const float dl_slots_per_s = (float)get_dl_slots_per_period(fs) / fs->numb_slots_period
                                * fs->numb_slots_frame * 100;

  UE_iterator(UE_list, UE) {
    if (n >= max_candidates)
      break;

    if (!nr_mac_ue_is_active(UE))
      continue;

    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *current_BWP = &UE->current_DL_BWP;
    NR_mac_dir_stats_t *stats = &UE->mac_stats.dl;

    /* Per-slot DL goodput EWMA (bps). current_bytes was set by last slot's dispatch. */
    float instant_bps = (float)stats->current_bytes * 8.0f * dl_slots_per_s;
    UE->dl_thr_ue = (1 - 0.01f) * UE->dl_thr_ue + 0.01f * instant_bps;

    stats->current_bytes = 0;
    stats->current_rbs = 0;

    /* Check TA */
    if (frame == sched_ctrl->ta_frame)
      sched_ctrl->ta_apply = true;

    int harq_pid = sched_ctrl->retrans_dl_harq.head;
    const NR_bler_options_t *bo = &mac->dl_bler;
    bwp_info_t bwp_info = get_pdsch_bwp_start_size(mac, UE);
    const int max_mcs_table = current_BWP->mcsTableIdx == 1 ? 27 : 28;
    const int max_mcs = min(sched_ctrl->dl_max_mcs, min(max_mcs_table, bo->max_mcs));

    /* QoS / slice info — extract from first DRB for external policies */
    uint64_t fiveQI = 0;
    int lc_priority = 0;
    uint8_t nssai_sst = 0;
    uint32_t nssai_sd = 0xFFFFFF;
    for (int j = 0; j < seq_arr_size(&sched_ctrl->lc_config); j++) {
      const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, j);
      if (c->lcid >= 4) {
        lc_priority = c->priority;
        nssai_sst = c->nssai.sst;
        nssai_sd = c->nssai.sd;
        for (int q = 0; q < NR_MAX_NUM_QFI; q++) {
          if (c->qos_config[q].fiveQI > 0) {
            fiveQI = c->qos_config[q].fiveQI;
            break;
          }
        }
        if (fiveQI > 0) break;
      }
    }
    uint16_t cqi = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.wb_cqi_1tb;
    uint8_t csi_ri = (current_BWP->dci_format == NR_DL_DCI_FORMAT_1_0)
                     ? 0 : sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.ri;
    int csi_pm_index = get_pm_index(mac, UE, current_BWP->dci_format, csi_ri + 1,
                                    mac->radio_config.pdsch_AntennaPorts.XP);

    if (harq_pid >= 0) {
      /* retransmission candidate */
      const NR_UE_harq_t *harq = &sched_ctrl->harq_processes[harq_pid];
      candidates[n++] = (nr_dl_candidate_t){
        /* identity / scheduling state */
        .UE = UE,
        .rnti = UE->rnti,
        .is_retx = true,
        .retx_harq_pid = harq_pid,
        .retx_rbSize = harq->sched_pdsch.rbSize,
        .avg_throughput = UE->dl_thr_ue,
        .bler = sched_ctrl->dl_bler_stats.bler,
        .current_mcs = harq->sched_pdsch.mcs,
        .max_mcs = max_mcs,
        .mcs_table = current_BWP->mcsTableIdx,
        .bwp_start = bwp_info.bwpStart,
        .bwp_size = bwp_info.bwpSize,
        .fiveQI = fiveQI,
        .priority = lc_priority,
        .nssai_sst = nssai_sst,
        .nssai_sd = nssai_sd,
        /* CSI observations */
        .cqi = cqi,
        .csi_ri = csi_ri,
        .csi_pm_index = csi_pm_index,
        .beam_rsrp = UE->beam_rsrp,
        .beam_sinr = UE->beam_sinr,
        /* alloc decisions — zeroed here, written by pipeline stages */
        .alloc_nrOfLayers = 0,
        .alloc_beam_dir = UE->UE_beam_index,
        .alloc_beam_idx = 0,
        .alloc_new_beam = false,
      };
    } else {
      /* new transmission candidate */
      if (sched_ctrl->available_dl_harq.head < 0)
        continue;

      update_dlsch_buffer(frame, slot, UE);

      if (!dlsch_to_schedule(sched_ctrl))
        continue;

      /* Update BLER stats; MCS adaptation is done by dl_mcs_select for all candidates. */
      bool bler_updated = update_bler_stats(bo, stats, &sched_ctrl->dl_bler_stats, frame);

      candidates[n++] = (nr_dl_candidate_t){
        /* identity / scheduling state */
        .UE = UE,
        .rnti = UE->rnti,
        .is_retx = false,
        .retx_harq_pid = -1,
        .pending_bytes = sched_ctrl->num_total_bytes,
        .avg_throughput = UE->dl_thr_ue,
        .bler = sched_ctrl->dl_bler_stats.bler,
        .current_mcs = min(sched_ctrl->dl_bler_stats.mcs, max_mcs),
        .max_mcs = max_mcs,
        .last_num_sched = sched_ctrl->dl_bler_stats.last_num_sched,
        .bler_updated = bler_updated,
        .mcs_table = current_BWP->mcsTableIdx,
        .bwp_start = bwp_info.bwpStart,
        .bwp_size = bwp_info.bwpSize,
        .fiveQI = fiveQI,
        .priority = lc_priority,
        .nssai_sst = nssai_sst,
        .nssai_sd = nssai_sd,
        /* CSI observations */
        .cqi = cqi,
        .csi_ri = csi_ri,
        .csi_pm_index = csi_pm_index,
        .beam_rsrp = UE->beam_rsrp,
        .beam_sinr = UE->beam_sinr,
        /* alloc decisions — zeroed here, written by pipeline stages */
        .alloc_nrOfLayers = 0,
        .alloc_beam_dir = UE->UE_beam_index,
        .alloc_beam_idx = 0,
        .alloc_new_beam = false,
      };
    }
  }

  return n;
}

void nr_dl_ri_pmi_select_default(const gNB_MAC_INST *mac,
                                 nr_dl_candidate_t *candidates,
                                 int n_candidates)
{
  for (int i = 0; i < n_candidates; i++) {
    nr_dl_candidate_t *cand = &candidates[i];
    NR_UE_sched_ctrl_t *sched_ctrl = &cand->UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &cand->UE->current_DL_BWP;
    if (cand->is_retx) {
      cand->alloc_nrOfLayers = sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch.nrOfLayers;
      cand->alloc_pm_index = get_pm_index(mac, cand->UE, dl_bwp->dci_format, cand->alloc_nrOfLayers,
                                          mac->radio_config.pdsch_AntennaPorts.XP);
    } else {
      cand->alloc_nrOfLayers = cand->csi_ri + 1;
      cand->alloc_pm_index = cand->csi_pm_index;
    }
  }
}

static float dl_pf_weight(int mcs, int mcs_table, int nrOfLayers, float avg_throughput)
{
  uint8_t Qm = nr_get_Qm_dl(mcs, mcs_table);
  uint16_t R = nr_get_code_rate_dl(mcs, mcs_table);
  uint32_t tbs = nr_compute_tbs(Qm, R, 1, 10, 0, 0, 0, nrOfLayers) >> 3;
  return (float)tbs / fmaxf(avg_throughput, 1e-9f);
}

static int compare_dl_beam_priority(const void *a, const void *b)
{
  const nr_dl_candidate_t *ca = (const nr_dl_candidate_t *)a;
  const nr_dl_candidate_t *cb = (const nr_dl_candidate_t *)b;
  /* retx first */
  if (ca->is_retx && !cb->is_retx)
    return -1;
  if (!ca->is_retx && cb->is_retx)
    return 1;
  /* among new-tx: highest PF weight first (use current_mcs — alloc_mcs not yet set) */
  float wa = dl_pf_weight(ca->current_mcs, ca->mcs_table, ca->alloc_nrOfLayers, ca->avg_throughput);
  float wb = dl_pf_weight(cb->current_mcs, cb->mcs_table, cb->alloc_nrOfLayers, cb->avg_throughput);
  return (wa < wb) - (wa > wb);
}

int nr_dl_beam_select_default(NR_beam_info_t *beam_info,
                             const int16_t *beam_index_list,
                             nr_dl_candidate_t *candidates,
                             int n_candidates,
                             frame_t frame, slot_t slot,
                             int slots_per_frame)
{
  /* Sort candidates so retx claim beam slots first, then new-tx in PF
   * priority order (lowest avg throughput first). */
  qsort(candidates, n_candidates, sizeof(*candidates), compare_dl_beam_priority);

  int out = 0;
  for (int i = 0; i < n_candidates; i++) {
    NR_beam_alloc_t beam = beam_allocation_procedure(beam_info, frame, slot,
                                                     candidates[i].alloc_beam_dir,
                                                     slots_per_frame);
    if (beam.idx < 0)
      continue;

    candidates[i].alloc_beam_idx = beam.idx;
    candidates[i].alloc_new_beam = beam.new_beam;
    if (out != i)
      candidates[out] = candidates[i];
    out++;
  }
  return out;
}

// Default TDA selector: picks the slot-wide TDA index from get_dl_tda(),
// then resolves tda_info per candidate using each UE's own BWP / search
// space / coreset. Compacts out candidates whose resolved TDA is invalid.
int nr_dl_tda_select_default(const gNB_MAC_INST *mac,
                             nr_dl_candidate_t *candidates,
                             int n_candidates,
                             frame_t frame, slot_t slot)
{
  int tda = get_dl_tda(mac, slot);
  AssertFatal(tda >= 0, "Unable to find PDSCH time domain allocation in list\n");
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;

  int out = 0;
  for (int i = 0; i < n_candidates; i++) {
    NR_UE_info_t *UE = candidates[i].UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
    int coresetid = sched_ctrl->coreset->controlResourceSetId;
    NR_tda_info_t tda_info = get_dl_tda_info(dl_bwp,
                                             sched_ctrl->search_space->searchSpaceType->present,
                                             tda, scc->dmrs_TypeA_Position,
                                             1, TYPE_C_RNTI_, coresetid, false);
    if (!tda_info.valid_tda)
      continue;

    candidates[i].alloc_tda = tda;
    candidates[i].alloc_tda_info = tda_info;
    candidates[i].alloc_slbitmap = SL_to_bitmap(tda_info.startSymbolIndex, tda_info.nrOfSymbols);
    if (out != i)
      candidates[out] = candidates[i];
    out++;
  }
  return out;
}

void nr_dl_mcs_select_default(const gNB_MAC_INST *mac,
                             nr_dl_candidate_t *candidates,
                             int n_candidates)
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
      mcs = nr_adapt_mcs_from_bler(cand->current_mcs, bo->min_mcs, cand->max_mcs,
                                   cand->bler, bo->lower, bo->upper, cand->last_num_sched);
    }
    cand->alloc_mcs = mcs;
    /* Persist for all candidates — BLER-based MCS ramps even for UEs the
     * policy rejects this slot (failed CCE, no free RBs, etc.). */
    if (!cand->is_retx)
      cand->UE->UE_sched_ctrl.dl_bler_stats.mcs = mcs;
  }
}

/* ---- CCE/PUCCH validation, called from inside the scheduling policy ---- */

static bool nr_dl_validate_cce_pucch(const nr_dl_sched_params_t *params,
                                     nr_dl_candidate_t *cand)
{
  NR_UE_info_t *UE = cand->UE;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

  int agg_level = sched_ctrl->aggregation_level;
  NR_sched_pdcch_t sched_pdcch = sched_ctrl->sched_pdcch;
  int CCEIndex = get_cce_index(params->mac, params->CC_id, params->slot, UE->rnti,
                               &agg_level, cand->alloc_beam_idx,
                               sched_ctrl->search_space, sched_ctrl->coreset,
                               &sched_pdcch, sched_ctrl->pdcch_cl_adjust);
  if (CCEIndex < 0) {
    sched_ctrl->dl_cce_fail++;
    LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find free CCE for DL DCI\n",
          UE->rnti, params->frame, params->slot);
    return false;
  }

  int harq_pid = cand->is_retx ? cand->retx_harq_pid
                                : sched_ctrl->available_dl_harq.head;
  int pucch_alloc = -1;
  if (!get_FeedbackDisabled(UE->sc_info.downlinkHARQ_FeedbackDisabled_r17, harq_pid)) {
    NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;
    int r_pucch = nr_get_pucch_resource(sched_ctrl->coreset, ul_bwp->pucch_Config, CCEIndex);
    pucch_alloc = nr_acknack_scheduling(params->mac, UE, params->frame, params->slot,
                                        UE->UE_beam_index, r_pucch, 0);
    if (pucch_alloc < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find PUCCH for DL DCI\n",
            UE->rnti, params->frame, params->slot);
      return false;
    }
  }

  cand->alloc_cce_index = CCEIndex;
  cand->alloc_aggregation_level = agg_level;
  cand->alloc_pucch_allocation = pucch_alloc;
  cand->alloc_sched_pdcch = sched_pdcch;
  return true;
}

// Validate CCE/PUCCH and mark VRBs for a candidate whose alloc_rbStart/rbSize/mcs
// have already been set. Returns false if validation fails (UE should be skipped).
static bool commit_alloc(const nr_dl_sched_params_t *params,
                         nr_dl_candidate_t *cand)
{
  if (!nr_dl_validate_cce_pucch(params, cand))
    return false;

  int beam = cand->alloc_beam_idx;

  /* Mark CCE as used so subsequent UEs in the same slot see it as taken */
  fill_pdcch_vrb_map(params->mac, params->CC_id, &cand->alloc_sched_pdcch,
                     cand->alloc_cce_index, cand->alloc_aggregation_level, beam);

  uint16_t *vrb_map = params->vrb_map[beam];
  for (int rb = 0; rb < cand->alloc_rbSize; rb++)
    vrb_map[cand->alloc_rbStart + rb + cand->bwp_start] |= cand->alloc_slbitmap;

  return true;
}

// Use inside the policy loops: sets RB/MCS on candidate, validates CCE/PUCCH,
// records index; continues on failure, returns on max_num_ue.
#define COMMIT_ALLOC(params, idx, cand, rbStart, rbSize, mcs, sched_indices, n_sched) \
  do {                                                                                 \
    (cand)->alloc_rbStart = (rbStart);                                                 \
    (cand)->alloc_rbSize  = (rbSize);                                                  \
    (cand)->alloc_mcs     = (mcs);                                                     \
    if (!commit_alloc(params, cand))                                                   \
      continue;                                                                        \
    (sched_indices)[(n_sched)++] = (idx);                                              \
    if ((n_sched) >= (params)->max_num_ue)                                             \
      return (n_sched);                                                                \
  } while (0)

typedef struct { float weight; int idx; } pf_entry_t;

static int compare_pf_weight_desc(const void *a, const void *b)
{
  float wa = ((const pf_entry_t *)a)->weight;
  float wb = ((const pf_entry_t *)b)->weight;
  return (wa < wb) - (wa > wb);
}

static int compare_dl_beam_idx(const void *a, const void *b)
{
  return ((const nr_dl_candidate_t *)a)->alloc_beam_idx - ((const nr_dl_candidate_t *)b)->alloc_beam_idx;
}

int nr_dl_proportional_fair(const nr_dl_sched_params_t *params,
                            nr_dl_candidate_t *candidates,
                            int *sched_indices,
                            int n_candidates)
{
  const int min_rbSize = 5;
  int n_scheduled = 0;

  /* Sort candidates by beam so each beam's slice is contiguous */
  qsort(candidates, n_candidates, sizeof(*candidates), compare_dl_beam_idx);

  /* Compute per-beam boundaries */
  int beam_start[MAX_NUM_BEAM_PERIODS + 1];
  beam_start[0] = 0;
  for (int b = 0; b < params->num_beams; b++) {
    beam_start[b + 1] = beam_start[b];
    while (beam_start[b + 1] < n_candidates && candidates[beam_start[b + 1]].alloc_beam_idx == b)
      beam_start[b + 1]++;
  }

  for (int b = 0; b < params->num_beams; b++) {
    int bs = beam_start[b];
    int be = beam_start[b + 1];
    if (bs == be)
      continue;
    uint16_t *vrb_map = params->vrb_map[b];

    /* Phase 1: HARQ retransmissions (highest priority, exact RBs) */
    for (int i = bs; i < be; i++) {
      if (!candidates[i].is_retx)
        continue;

      int needed_rbs = candidates[i].retx_rbSize;
      int rbStart, rbSize;
      if (!get_rb_alloc(needed_rbs, candidates[i].bwp_size, candidates[i].bwp_start,
                        candidates[i].bwp_size, vrb_map, candidates[i].alloc_slbitmap,
                        &rbStart, &rbSize))
        continue;

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, needed_rbs,
                   candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 2: No-data UEs (TA command or beam switch MAC CE, no RLC data) */
    for (int i = bs; i < be; i++) {
      if (candidates[i].is_retx || candidates[i].pending_bytes > 0)
        continue;

      int rbStart, rbSize;
      if (!get_rb_alloc(min_rbSize, candidates[i].bwp_size, candidates[i].bwp_start,
                        candidates[i].bwp_size, vrb_map, candidates[i].alloc_slbitmap,
                        &rbStart, &rbSize))
        continue;

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, min_rbSize,
                   candidates[i].alloc_mcs, sched_indices, n_scheduled);
    }

    /* Phase 3: New data UEs — PF priority, largest free block */
    pf_entry_t entries[MAX_MOBILES_PER_GNB];
    int n_new = 0;

    for (int i = bs; i < be; i++) {
      if (candidates[i].is_retx || candidates[i].pending_bytes == 0)
        continue;

      float weight = dl_pf_weight(candidates[i].alloc_mcs, candidates[i].mcs_table,
                                  candidates[i].alloc_nrOfLayers, candidates[i].avg_throughput);

      entries[n_new].weight = weight;
      entries[n_new].idx = i;
      n_new++;
    }

    /* Sort by PF weight descending */
    qsort(entries, n_new, sizeof(pf_entry_t), compare_pf_weight_desc);

    /* Allocate in priority order: find largest free block, cap to UE's needs */
    for (int e = 0; e < n_new; e++) {
      int i = entries[e].idx;
      int rbStart;
      int max_rbSize = find_largest_free_block(vrb_map, candidates[i].alloc_slbitmap,
                                               candidates[i].bwp_start, candidates[i].bwp_size,
                                               &rbStart);
      if (max_rbSize < min_rbSize)
        break;

      /* Size allocation to UE's actual needs (RLC bytes + MAC overhead) */
      int mcs = candidates[i].alloc_mcs;
      uint8_t Qm = nr_get_Qm_dl(mcs, candidates[i].mcs_table);
      uint16_t R = nr_get_code_rate_dl(mcs, candidates[i].mcs_table);
      NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(params->mac->common_channels->ServingCellConfigCommon,
                                                 &candidates[i].UE->current_DL_BWP,
                                                 &candidates[i].alloc_tda_info, candidates[i].alloc_nrOfLayers);
      const int oh = 3 * 4 + (candidates[i].UE->UE_sched_ctrl.ta_apply ? 2 : 0);
      uint32_t tbs;
      uint16_t rbSize;
      nr_find_nb_rb(Qm, R, 1, candidates[i].alloc_nrOfLayers,
                    candidates[i].alloc_tda_info.nrOfSymbols,
                    dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                    candidates[i].pending_bytes + oh,
                    min_rbSize, max_rbSize,
                    &tbs, &rbSize);

      COMMIT_ALLOC(params, i, &candidates[i], rbStart, rbSize,
                   mcs, sched_indices, n_scheduled);
    }
  }

  return n_scheduled;
}


static void nr_dl_schedule(gNB_MAC_INST *mac,
                  post_process_pdsch_t *pp_pdsch,
                  NR_UE_info_t **UE_list,
                  int max_num_ue,
                  int num_beams,
                  int n_rb_sched[num_beams])
{
  frame_t frame = pp_pdsch->frame;
  slot_t slot = pp_pdsch->slot;
  int CC_id = 0;
  NR_ServingCellConfigCommon_t *scc = mac->common_channels[CC_id].ServingCellConfigCommon;
  int slots_per_frame = mac->frame_structure.numb_slots_frame;

  /* Step 1: Collect candidates */
  nr_dl_candidate_t candidates[MAX_MOBILES_PER_GNB];
  int n = collect_dl_candidates(mac, UE_list, candidates, MAX_MOBILES_PER_GNB, frame, slot);
  if (n == 0)
    return;

  /* Step 2: RI/PMI selection — sets alloc_nrOfLayers and alloc_pm_index per candidate */
  mac->dl_ri_pmi_select(mac, candidates, n);

  /* Step 3: Per-UE TDA selection. Resolves tda/tda_info/slbitmap on each
   * candidate using its own BWP/search-space/coreset, compacts out invalids.
   * Done before beam allocation so tda-invalid candidates don't claim
   * beam reservations they'll never use. */
  n = mac->dl_tda_select(mac, candidates, n, frame, slot);
  if (n == 0)
    return;

  /* Step 4: Beam allocation (skip for single beam — candidates default to beam 0) */
  if (num_beams > 1) {
    n = mac->dl_beam_select(&mac->beam_info, mac->beam_index_list, candidates, n, frame, slot, slots_per_frame);
    if (n == 0)
      return;
  }

  /* Step 5: MCS adaptation — sets alloc_mcs from BLER state for all surviving candidates.
   * Placed after beam allocation so a custom dl_mcs_select can factor in alloc_beam_dir
   * (e.g. use beam RSRP/SINR to bias the MCS target). Also persists the decision so
   * BLER-based MCS ramps even for candidates the policy rejects this slot. */
  mac->dl_mcs_select(mac, candidates, n);

  /* Step 6: RB allocation policy (beam loop is inside the policy) */
  int sched_indices[MAX_MOBILES_PER_GNB];

  nr_dl_sched_params_t params = {
    .mac = mac,
    .CC_id = CC_id,
    .frame = frame,
    .slot = slot,
    .num_beams = num_beams,
    .max_num_ue = max_num_ue,
    .min_mcs = mac->dl_bler.min_mcs,
    .bler_lower = mac->dl_bler.lower,
    .bler_upper = mac->dl_bler.upper,
  };
  for (int b = 0; b < num_beams; b++) {
    params.vrb_map[b] = mac->common_channels[CC_id].vrb_map[b];
    params.n_rb_avail[b] = n_rb_sched[b];
  }

  int n_allocs = mac->dl_rb_alloc(&params, candidates, sched_indices, n);

  /* Release beam reservations for candidates the policy rejected (failed
   * CCE/PUCCH validation, no free RBs, max_num_ue reached, etc.).
   * No-op in single-beam mode where alloc_new_beam is always false. */
  bool scheduled[MAX_MOBILES_PER_GNB] = {0};
  for (int j = 0; j < n_allocs; j++)
    scheduled[sched_indices[j]] = true;
  for (int i = 0; i < n; i++) {
    if (!scheduled[i])
      reset_beam_status(&mac->beam_info, frame, slot,
                        candidates[i].alloc_beam_dir, slots_per_frame,
                        candidates[i].alloc_new_beam);
  }

  /* Step 7: Persist MCS, apply CCE/PUCCH, compute TBS, post_process */
  for (int j = 0; j < n_allocs; j++) {
    nr_dl_candidate_t *cand = &candidates[sched_indices[j]];
    NR_UE_info_t *UE = cand->UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;

    /* TDA was resolved per-cand by dl_tda_select; no recomputation. */
    int tda = cand->alloc_tda;
    NR_tda_info_t tda_info = cand->alloc_tda_info;

    /* Reuse CCE/PUCCH from commit_alloc — no recomputation */
    sched_ctrl->cce_index = cand->alloc_cce_index;
    sched_ctrl->aggregation_level = cand->alloc_aggregation_level;
    sched_ctrl->sched_pdcch = cand->alloc_sched_pdcch;
    int alloc = cand->alloc_pucch_allocation;
    /* fill_pdcch_vrb_map already called in commit_alloc */

    int l = cand->alloc_nrOfLayers;
    bwp_info_t bwp_info = {.bwpStart = cand->bwp_start, .bwpSize = cand->bwp_size};

    int mcs = cand->alloc_mcs; /* MCS already adapted by the policy */

    NR_sched_pdsch_t sched_pdsch;
    if (cand->is_retx) {
      /* Retransmission: reuse stored sched_pdsch, override placement */
      sched_pdsch = sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch;
      sched_pdsch.rbStart = cand->alloc_rbStart;
      sched_pdsch.pucch_allocation = alloc;
      sched_pdsch.bwp_info = bwp_info;
      sched_pdsch.dl_harq_pid = cand->retx_harq_pid;
    } else {
      /* New transmission: build from policy allocation */
      uint8_t Qm = nr_get_Qm_dl(mcs, dl_bwp->mcsTableIdx);
      uint16_t R = nr_get_code_rate_dl(mcs, dl_bwp->mcsTableIdx);
      NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(scc, dl_bwp, &tda_info, l);

      sched_pdsch = (NR_sched_pdsch_t){
        .rbStart = cand->alloc_rbStart,
        .mcs = mcs,
        .R = R,
        .Qm = Qm,
        .dl_harq_pid = sched_ctrl->available_dl_harq.head,
        .pucch_allocation = alloc,
        .pm_index = cand->alloc_pm_index,
        .nrOfLayers = l,
        .bwp_info = bwp_info,
        .dmrs_parms = dmrs,
        .time_domain_allocation = tda,
        .tda_info = tda_info,
      };

      /* Compute actual TBS (policy allocated max rbSize, nr_find_nb_rb gives actual) */
      const int oh = 3 * 4 + (sched_ctrl->ta_apply ? 2 : 0);
      nr_find_nb_rb(Qm, R, 1, l,
                    tda_info.nrOfSymbols,
                    dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                    sched_ctrl->num_total_bytes + oh,
                    5, cand->alloc_rbSize,
                    &sched_pdsch.tb_size, &sched_pdsch.rbSize);

      sched_pdsch.action = NULL;
      int srb1 = 1;
      if (UE->reconfigCellGroup && sched_ctrl->rlc_status[srb1].bytes_in_buffer > 10)
        sched_pdsch.action = ack_reconfig;
    }

    post_process_dlsch(mac, pp_pdsch, UE, &sched_pdsch);
  }
}

void nr_dlsch_preprocessor(gNB_MAC_INST *mac, post_process_pdsch_t *pp_pdsch)
{
  NR_UEs_t *UE_info = &mac->UE_info;

  if (UE_info->connected_ue_list[0] == NULL)
    return;

  NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  int bw = scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;
  int num_beams = mac->beam_info.beam_allocation ? mac->beam_info.beams_per_period : 1;
  int n_rb_sched[num_beams];
  for (int i = 0; i < num_beams; i++)
    n_rb_sched[i] = bw;

  int average_agg_level = 4; // TODO find a better estimation
  int max_sched_ues = bw / (average_agg_level * NR_NB_REG_PER_CCE);

  // FAPI cannot handle more than MAX_DCI_CORESET DCIs
  max_sched_ues = min(max_sched_ues, MAX_DCI_CORESET);

  nr_dl_schedule(mac, pp_pdsch, UE_info->connected_ue_list, max_sched_ues, num_beams, n_rb_sched);
}


nfapi_nr_dl_tti_pdsch_pdu_rel15_t *prepare_pdsch_pdu(nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdsch_pdu,
                                                     const gNB_MAC_INST *mac,
                                                     const NR_UE_info_t *UE,
                                                     const NR_sched_pdsch_t *sched_pdsch,
                                                     const NR_PDSCH_Config_t *pdsch_Config,
                                                     bool is_sib1,
                                                     int harq_round,
                                                     int rnti,
                                                     int beam_index,
                                                     int nl_tbslbrm,
                                                     int pdu_index)
{
  const NR_UE_DL_BWP_t *dl_bwp = UE ? &UE->current_DL_BWP : NULL;
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *pdsch_pdu = &dl_tti_pdsch_pdu->pdsch_pdu.pdsch_pdu_rel15;
  pdsch_pdu->pduBitmap = 0;
  pdsch_pdu->rnti = rnti;
  pdsch_pdu->pduIndex = pdu_index;
  pdsch_pdu->BWPSize  = sched_pdsch->bwp_info.bwpSize;
  pdsch_pdu->BWPStart = sched_pdsch->bwp_info.bwpStart;
  pdsch_pdu->SubcarrierSpacing = dl_bwp ? dl_bwp->scs : *scc->ssbSubcarrierSpacing;
  pdsch_pdu->CyclicPrefix = dl_bwp && dl_bwp->cyclicprefix ? *dl_bwp->cyclicprefix : 0;
  // Codeword information
  pdsch_pdu->NrOfCodewords = 1;
  pdsch_pdu->targetCodeRate[0] = sched_pdsch->R;
  pdsch_pdu->qamModOrder[0] = sched_pdsch->Qm;
  pdsch_pdu->mcsIndex[0] = sched_pdsch->mcs;
  pdsch_pdu->mcsTable[0] = dl_bwp ? dl_bwp->mcsTableIdx : 0;
  pdsch_pdu->rvIndex[0] = nr_get_rv(harq_round % 4);
  pdsch_pdu->TBSize[0] = sched_pdsch->tb_size;
  pdsch_pdu->dataScramblingId = pdsch_Config && pdsch_Config->dataScramblingIdentityPDSCH ? *pdsch_Config->dataScramblingIdentityPDSCH : *scc->physCellId;
  pdsch_pdu->nrOfLayers = sched_pdsch->nrOfLayers;
  pdsch_pdu->transmissionScheme = 0;
  pdsch_pdu->refPoint = is_sib1;
  // DMRS
  const NR_pdsch_dmrs_t *dmrs_parms = &sched_pdsch->dmrs_parms;
  pdsch_pdu->dlDmrsSymbPos = dmrs_parms->dl_dmrs_symb_pos;
  pdsch_pdu->dmrsConfigType = dmrs_parms->dmrsConfigType;
  pdsch_pdu->SCID = dmrs_parms->n_scid;
  pdsch_pdu->dlDmrsScramblingId = dmrs_parms->scrambling_id;
  pdsch_pdu->numDmrsCdmGrpsNoData = dmrs_parms->numDmrsCdmGrpsNoData;
  pdsch_pdu->dmrsPorts = (1 << sched_pdsch->nrOfLayers) - 1;  // FIXME with a better implementation
  // Pdsch Allocation in frequency domain
  pdsch_pdu->resourceAlloc = 1;
  pdsch_pdu->rbStart = sched_pdsch->rbStart;
  pdsch_pdu->rbSize = sched_pdsch->rbSize;
  pdsch_pdu->VRBtoPRBMapping = 0; // non-interleaved
  // Resource Allocation in time domain
  const NR_tda_info_t *tda_info = &sched_pdsch->tda_info;
  pdsch_pdu->StartSymbolIndex = tda_info->startSymbolIndex;
  pdsch_pdu->NrOfSymbols = tda_info->nrOfSymbols;
  /* Check and validate PTRS values */
  if (dmrs_parms->phaseTrackingRS) {
    bool valid_ptrs_setup = set_dl_ptrs_values(dmrs_parms->phaseTrackingRS,
                                               pdsch_pdu->rbSize,
                                               pdsch_pdu->mcsIndex[0],
                                               pdsch_pdu->mcsTable[0],
                                               &pdsch_pdu->PTRSFreqDensity,
                                               &pdsch_pdu->PTRSTimeDensity,
                                               &pdsch_pdu->PTRSPortIndex,
                                               &pdsch_pdu->nEpreRatioOfPDSCHToPTRS,
                                               &pdsch_pdu->PTRSReOffset,
                                               pdsch_pdu->NrOfSymbols);
    if (valid_ptrs_setup)
      pdsch_pdu->pduBitmap |= 0x1; // Bit 0: pdschPtrs - Indicates PTRS included (FR2)
  }
  int dl_bw_tbslbrm = UE ? UE->sc_info.dl_bw_tbslbrm : sched_pdsch->bwp_info.bwpSize;
  pdsch_pdu->maintenance_parms_v3.tbSizeLbrmBytes = nr_compute_tbslbrm(pdsch_pdu->mcsTable[0], dl_bw_tbslbrm, nl_tbslbrm);
  pdsch_pdu->maintenance_parms_v3.ldpcBaseGraph = get_BG(sched_pdsch->tb_size << 3, sched_pdsch->R);
  // Precoding and beamforming
  pdsch_pdu->precodingAndBeamforming.num_prgs = 1;
  pdsch_pdu->precodingAndBeamforming.prg_size = pdsch_pdu->rbSize;
  pdsch_pdu->precodingAndBeamforming.dig_bf_interfaces = 1;
  pdsch_pdu->precodingAndBeamforming.prgs_list[0].pm_idx = sched_pdsch->pm_index; 
  pdsch_pdu->precodingAndBeamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = beam_index;
  return pdsch_pdu;
}

// Resolve HARQ PID, remove from available/retrans list, set up feedback.
// Sets sched_pdsch->dl_harq_pid. Returns the harq process pointer.
static NR_UE_harq_t *setup_dl_harq_process(NR_UE_sched_ctrl_t *sched_ctrl,
                                            NR_sched_pdsch_t *sched_pdsch,
                                            rnti_t rnti)
{
  int8_t current_harq_pid = sched_pdsch->dl_harq_pid;

  if (current_harq_pid < 0) {
    /* PP has not selected a specific HARQ Process, get a new one */
    current_harq_pid = sched_ctrl->available_dl_harq.head;
    AssertFatal(current_harq_pid >= 0,
                "no free HARQ process available for UE %04x\n",
                rnti);
    remove_front_nr_list(&sched_ctrl->available_dl_harq);
    sched_pdsch->dl_harq_pid = current_harq_pid;
  } else {
    /* PP selected a specific HARQ process. Check whether it will be a new
     * transmission or a retransmission, and remove from the corresponding
     * list */
    if (sched_ctrl->harq_processes[current_harq_pid].round == 0)
      remove_nr_list(&sched_ctrl->available_dl_harq, current_harq_pid);
    else
      remove_nr_list(&sched_ctrl->retrans_dl_harq, current_harq_pid);
  }

  NR_UE_harq_t *harq = &sched_ctrl->harq_processes[current_harq_pid];
  DevAssert(!harq->is_waiting);
  if (sched_pdsch->pucch_allocation < 0) {
    finish_nr_dl_harq(sched_ctrl, current_harq_pid);
  } else {
    NR_sched_pucch_t *pucch = &sched_ctrl->sched_pucch[sched_pdsch->pucch_allocation];
    add_tail_nr_list(&sched_ctrl->feedback_dl_harq, current_harq_pid);
    harq->feedback_frame = pucch->frame;
    harq->feedback_slot = pucch->ul_slot;
    harq->is_waiting = true;
  }
  return harq;
}

static void generate_dl_mac_pdu(gNB_MAC_INST *mac,
                                NR_UE_info_t *UE,
                                NR_UE_harq_t *harq,
                                NR_sched_pdsch_t *sched_pdsch,
                                frame_t frame,
                                slot_t slot)
{
  const int module_id = mac->Mod_id;
  const rnti_t rnti = UE->rnti;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  const int8_t current_harq_pid = sched_pdsch->dl_harq_pid;
  const uint32_t TBS = sched_pdsch->tb_size;

  if (harq->round != 0) { /* retransmission */
    /* we do not have to do anything, since we do not require to get data
     * from RLC or encode MAC CEs. The TX_req structure is filled below
     * or copy data to FAPI structures */
    LOG_D(NR_MAC,
          "%d.%2d DL retransmission RNTI %04x HARQ PID %d round %d NDI %d\n",
          frame,
          slot,
          rnti,
          current_harq_pid,
          harq->round,
          harq->ndi);
    if (!get_softmodem_params()->phy_test)
      AssertFatal(harq->sched_pdsch.tb_size == TBS,
                  "UE %04x mismatch between scheduled TBS and buffered TB for HARQ PID %d\n",
                  UE->rnti,
                  current_harq_pid);
    else if (harq->sched_pdsch.tb_size != TBS)
      LOG_E(NR_MAC,
            "Mismatch between scheduled TBS and buffered TB for HARQ PID %d. No RTX control in phy-test mode. "
            "Possible causes: presence of CSI-RS or DLSCH scheduled in the mixed slot.\n",
            current_harq_pid);
    T(T_GNB_MAC_RETRANSMISSION_DL_PDU_WITH_DATA, T_INT(module_id), T_INT(0), T_INT(rnti),
      T_INT(frame), T_INT(slot), T_INT(current_harq_pid), T_INT(harq->round), T_BUFFER(harq->transportBlock.buf, TBS));
    UE->mac_stats.dl.total_rbs_retx += sched_pdsch->rbSize;
    mac->mac_stats.dl.used_prb_aggregate += sched_pdsch->rbSize;
  } else { /* initial transmission */
    LOG_D(NR_MAC, "Initial HARQ transmission in %d.%d\n", frame, slot);
    // Flag HARQ process to start TCI timer at ACK
    harq->start_tci_timer = sched_ctrl->UE_mac_ce_ctrl.tci_state_ind.is_scheduled;
    uint8_t *buf = allocate_transportBlock_buffer(&harq->transportBlock, TBS);
    /* first, write all CEs that might be there */
    int written = nr_write_ce_dlsch_pdu(module_id,
                                        sched_ctrl,
                                        (unsigned char *)buf,
                                        255, // no drx
                                        NULL); // contention res id
    buf += written;
    uint8_t *bufEnd = buf + TBS - written;
    DevAssert(TBS > written);
    int dlsch_total_bytes = 0;
    /* next, get RLC data */
    start_meas(&mac->rlc_data_req);
    int sdus = 0;

    if (sched_ctrl->num_total_bytes > 0) {
      /* loop over all activated logical channels */
      for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
        const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
        const int lcid = c->lcid;

        if (sched_ctrl->rlc_status[lcid].bytes_in_buffer == 0)
          continue; // no data for this LC        tbs_size_t len = 0;

        int lcid_bytes=0;
        while (bufEnd-buf > sizeof(NR_MAC_SUBHEADER_LONG) + 1 ) {
          // we do not know how much data we will get from RLC, i.e., whether it
          // will be longer than 256B or not. Therefore, reserve space for long header, then
          // fetch data, then fill real length
          NR_MAC_SUBHEADER_LONG *header = (NR_MAC_SUBHEADER_LONG *) buf;
          /* limit requested number of bytes to what preprocessor specified, or
           * such that TBS is full */
          const rlc_buffer_occupancy_t ndata = min(sched_ctrl->rlc_status[lcid].bytes_in_buffer,
                                                   bufEnd-buf-sizeof(NR_MAC_SUBHEADER_LONG));
          tbs_size_t len = nr_mac_rlc_data_req(module_id,
                                               rnti,
                                               true,
                                               lcid,
                                               ndata,
                                               (char *)buf+sizeof(NR_MAC_SUBHEADER_LONG));
          LOG_D(NR_MAC,
                "%4d.%2d RNTI %04x: %d bytes from %s %d (ndata %d, remaining size %ld)\n",
                frame,
                slot,
                rnti,
                len,
                lcid < 4 ? "DCCH" : "DTCH",
                lcid,
                ndata,
                bufEnd-buf-sizeof(NR_MAC_SUBHEADER_LONG));

          if (len == 0)
            break;

          T(T_GNB_MAC_LCID_DL, T_INT(rnti), T_INT(frame), T_INT(slot), T_INT(lcid), T_INT(len * 8), T_INT(nr_rlc_tx_list_occupancy(rnti, lcid)));
          header->R = 0;
          header->F = 1;
          header->LCID = lcid;
          header->L = htons(len);
          buf += len+sizeof(NR_MAC_SUBHEADER_LONG);
          dlsch_total_bytes += len;
          lcid_bytes += len;
          sdus += 1;
        }

        UE->mac_stats.dl.lc_bytes[lcid] += lcid_bytes;
      }
    } else if (get_softmodem_params()->phy_test || get_softmodem_params()->do_ra) {
      /* we will need the large header, phy-test typically allocates all
       * resources and fills to the last byte below */
      LOG_D(NR_MAC, "Configuring DL_TX in %d.%d: TBS %d of random data\n", frame, slot, TBS);

      if (bufEnd-buf > sizeof(NR_MAC_SUBHEADER_LONG) ) {
        NR_MAC_SUBHEADER_LONG *header = (NR_MAC_SUBHEADER_LONG *) buf;
        // fill dlsch_buffer with random data
        header->R = 0;
        header->F = 1;
        header->LCID = DL_SCH_LCID_PADDING;
        buf += sizeof(NR_MAC_SUBHEADER_LONG);
        header->L = htons(bufEnd-buf);

        for (; ((intptr_t)buf) % 4; buf++)
          *buf = lrand48() & 0xff;
        for (; buf < bufEnd - 3; buf += 4) {
          uint32_t *buf32 = (uint32_t *)buf;
          *buf32 = lrand48();
        }
        for (; buf < bufEnd; buf++)
          *buf = lrand48() & 0xff;
        sdus +=1;
      }
    }

    stop_meas(&mac->rlc_data_req);

    // Add padding header and zero rest out if there is space left
    if (bufEnd-buf > 0) {
      NR_MAC_SUBHEADER_FIXED *padding = (NR_MAC_SUBHEADER_FIXED *) buf;
      padding->R = 0;
      padding->LCID = DL_SCH_LCID_PADDING;
      buf += 1;
      memset(buf,0,bufEnd-buf);
      buf=bufEnd;
    }

    UE->mac_stats.dl.total_bytes += TBS;
    UE->mac_stats.dl.current_bytes = TBS;
    UE->mac_stats.dl.total_rbs += sched_pdsch->rbSize;
    UE->mac_stats.dl.num_mac_sdu += sdus;
    UE->mac_stats.dl.current_rbs = sched_pdsch->rbSize;
    UE->mac_stats.dl.total_sdu_bytes += dlsch_total_bytes;
    mac->mac_stats.dl.used_prb_aggregate += sched_pdsch->rbSize;

    /* save retransmission information */
    harq->sched_pdsch = *sched_pdsch;
    /* save which time allocation has been used, to be used on
     * retransmissions */
    harq->sched_pdsch.time_domain_allocation = sched_pdsch->time_domain_allocation;

    // reset TCI state
    if (sched_ctrl->UE_mac_ce_ctrl.tci_state_ind.is_scheduled)
      sched_ctrl->UE_mac_ce_ctrl.tci_state_ind.is_scheduled = false;


    // ta command is sent, values are reset
    if (sched_ctrl->ta_apply) {
      sched_ctrl->ta_apply = false;
      sched_ctrl->ta_update = 31;
      sched_ctrl->ta_frame = (frame + 100) % MAX_FRAME_NUMBER;
      LOG_D(NR_MAC, "%d.%2d UE %04x TA scheduled, setting next TA frame to %d\n", frame, slot, UE->rnti, sched_ctrl->ta_frame);
    }

    T(T_GNB_MAC_DL_PDU_WITH_DATA, T_INT(module_id), T_INT(0), T_INT(rnti),
      T_INT(frame), T_INT(slot), T_INT(current_harq_pid), T_BUFFER(harq->transportBlock.buf, TBS));
    T(T_GNB_MAC_DL, T_INT(rnti), T_INT(frame), T_INT(slot), T_INT(sched_pdsch->mcs), T_INT(TBS));
  }
}

static void fill_dl_tx_request(post_process_pdsch_t *pdsch,
                               const uint8_t *buf,
                               int pduindex,
                               uint32_t TBS,
                               frame_t frame,
                               slot_t slot)
{
  const int ntx_req = pdsch->TX_req->Number_of_PDUs;
  nfapi_nr_pdu_t *tx_req = &pdsch->TX_req->pdu_list[ntx_req];
  tx_req->PDU_index  = pduindex;
  tx_req->num_TLV = 1;
  tx_req->TLVs[0].length = TBS;
  tx_req->PDU_length = compute_PDU_length(tx_req->num_TLV, tx_req->TLVs[0].length);
  memcpy(tx_req->TLVs[0].value.direct, buf, TBS);
  pdsch->TX_req->Number_of_PDUs++;
  pdsch->TX_req->SFN = frame;
  pdsch->TX_req->Slot = slot;
}

void post_process_dlsch(gNB_MAC_INST *nr_mac, post_process_pdsch_t *pdsch, NR_UE_info_t *UE, NR_sched_pdsch_t *sched_pdsch)
{
  int CC_id = 0;
  frame_t frame = pdsch->frame;
  slot_t slot = pdsch->slot;

  const NR_ServingCellConfigCommon_t *scc = nr_mac->common_channels[CC_id].ServingCellConfigCommon;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_DL_BWP_t *current_BWP = &UE->current_DL_BWP;

  const rnti_t rnti = UE->rnti;

  /* POST processing */
  const uint8_t nrOfLayers = sched_pdsch->nrOfLayers;
  const uint32_t TBS = sched_pdsch->tb_size;

  NR_UE_harq_t *harq = setup_dl_harq_process(sched_ctrl, sched_pdsch, rnti);
  int8_t current_harq_pid = sched_pdsch->dl_harq_pid;
  NR_tda_info_t *tda_info = &sched_pdsch->tda_info;
  NR_pdsch_dmrs_t *dmrs_parms = &sched_pdsch->dmrs_parms;
  NR_sched_pucch_t *pucch = sched_pdsch->pucch_allocation >= 0
                           ? &sched_ctrl->sched_pucch[sched_pdsch->pucch_allocation] : NULL;
  UE->mac_stats.dl.rounds[harq->round]++;
  int tpc = nr_mac_get_tpc(&sched_ctrl->pucch_pc);
  LOG_D(NR_MAC,
        "%4d.%2d [DLSCH/PDSCH/PUCCH] RNTI %04x DCI L %d start %3d RBs %3d startSymbol %2d nb_symbol %2d dmrspos %x MCS %2d nrOfLayers %d TBS %4d HARQ PID %2d round %d RV %d NDI %d dl_data_to_ULACK %d (%d.%d) PUCCH allocation %d TPC %d\n",
        frame,
        slot,
        rnti,
        sched_ctrl->aggregation_level,
        sched_pdsch->rbStart,
        sched_pdsch->rbSize,
        tda_info->startSymbolIndex,
        tda_info->nrOfSymbols,
        dmrs_parms->dl_dmrs_symb_pos,
        sched_pdsch->mcs,
        nrOfLayers,
        TBS,
        current_harq_pid,
        harq->round,
        nr_get_rv(harq->round % 4),
        harq->ndi,
        pucch ? pucch->timing_indicator : 0,
        pucch ? pucch->frame : 0,
        pucch ? pucch->ul_slot : 0,
        sched_pdsch->pucch_allocation,
        tpc);
  DevAssert(sched_pdsch->rbSize > 0);

  const int bwp_id = current_BWP->bwp_id;
  const int coresetid = sched_ctrl->coreset->controlResourceSetId;

  /* look up the PDCCH PDU for this CC, BWP, and CORESET. If it does not exist, create it */
  nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu = nr_mac->pdcch_pdu_idx[CC_id][coresetid];

  if (!pdcch_pdu) {
    LOG_D(NR_MAC, "creating pdcch pdu, pdcch_pdu = NULL. \n");
    nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdcch_pdu = &pdsch->dl_req->dl_tti_pdu_list[pdsch->dl_req->nPDUs];
    memset(dl_tti_pdcch_pdu, 0, sizeof(nfapi_nr_dl_tti_request_pdu_t));
    dl_tti_pdcch_pdu->PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
    dl_tti_pdcch_pdu->PDUSize = (uint8_t)(2+sizeof(nfapi_nr_dl_tti_pdcch_pdu));
    pdsch->dl_req->nPDUs += 1;
    pdcch_pdu = &dl_tti_pdcch_pdu->pdcch_pdu.pdcch_pdu_rel15;
    LOG_D(NR_MAC,"Trying to configure DL pdcch for UE %04x, bwp %d, cs %d\n", UE->rnti, bwp_id, coresetid);
    NR_ControlResourceSet_t *coreset = sched_ctrl->coreset;
    nr_configure_pdcch(pdcch_pdu, coreset, &sched_ctrl->sched_pdcch);
    nr_mac->pdcch_pdu_idx[CC_id][coresetid] = pdcch_pdu;
  }

  nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdsch_pdu = &pdsch->dl_req->dl_tti_pdu_list[pdsch->dl_req->nPDUs];
  memset(dl_tti_pdsch_pdu, 0, sizeof(nfapi_nr_dl_tti_request_pdu_t));
  dl_tti_pdsch_pdu->PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
  dl_tti_pdsch_pdu->PDUSize = (uint8_t)(2 + sizeof(nfapi_nr_dl_tti_pdsch_pdu));
  pdsch->dl_req->nPDUs += 1;

  /* SCF222: PDU index incremented for each PDSCH PDU sent in TX control
   * message. This is used to associate control information to data and is
   * reset every slot. */
  const int pduindex = nr_mac->pdu_index[CC_id]++;
  // TODO: verify the case where maxMIMO_Layers is NULL, in which case
  //       in principle maxMIMO_layers should be given by the maximum number of layers
  //       for PDSCH supported by the UE for the serving cell (5.4.2.1 of 38.212)
  long ue_supp_nl = ue_supported_dl_layers(scc, UE->capability);
  long maxMIMO_Layers = UE->sc_info.maxMIMO_Layers_PDSCH ? *UE->sc_info.maxMIMO_Layers_PDSCH : ue_supp_nl;
  if (maxMIMO_Layers < 1) {
    LOG_D(NR_MAC, "Both maxMIMO_Layers_PDSCH and UE supported layers are not present, defaulting to 1\n");
    maxMIMO_Layers = 1;
  }
  const int nl_tbslbrm = min(maxMIMO_Layers, 4);
  const uint16_t fapi_beam = convert_to_fapi_beam(UE->UE_beam_index, nr_mac->beam_info.beam_mode);
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *pdsch_pdu = prepare_pdsch_pdu(dl_tti_pdsch_pdu,
                                                                   nr_mac,
                                                                   UE,
                                                                   sched_pdsch,
                                                                   current_BWP->pdsch_Config,
                                                                   false,
                                                                   harq->round,
                                                                   rnti,
                                                                   fapi_beam,
                                                                   nl_tbslbrm,
                                                                   pduindex);


  LOG_D(NR_MAC,"Configuring DCI/PDCCH in %d.%d at CCE %d, rnti %x\n", frame,slot,sched_ctrl->cce_index,rnti);
  /* Fill PDCCH DL DCI PDU */
  nfapi_nr_dl_dci_pdu_t *dci_pdu = prepare_dci_pdu(pdcch_pdu,
                                                   scc,
                                                   sched_ctrl->search_space,
                                                   sched_ctrl->coreset,
                                                   sched_ctrl->aggregation_level,
                                                   sched_ctrl->cce_index,
                                                   fapi_beam,
                                                   rnti);
  pdcch_pdu->numDlDci++;

  /* DCI payload */
  const int rnti_type = TYPE_C_RNTI_;
  dci_pdu_rel15_t dci_payload = prepare_dci_dl_payload(nr_mac,
                                                       UE,
                                                       rnti_type,
                                                       sched_ctrl->search_space->searchSpaceType->present,
                                                       pdsch_pdu,
                                                       sched_pdsch,
                                                       pucch,
                                                       tpc,
                                                       current_harq_pid,
                                                       0,
                                                       false);

  NR_PDSCH_Config_t *pdsch_Config = current_BWP->pdsch_Config;
  AssertFatal(pdsch_Config == NULL
              || pdsch_Config->resourceAllocation == NR_PDSCH_Config__resourceAllocation_resourceAllocationType1,
              "Only frequency resource allocation type 1 is currently supported\n");

  LOG_D(NR_MAC,
        "%4d.%2d DCI type 1 payload: freq_alloc %d (%d,%d,%d), "
        "nrOfLayers %d, time_alloc %d, vrb to prb %d, mcs %d tb_scaling %d ndi %d rv %d tpc %d ti %d\n",
        frame,
        slot,
        dci_payload.frequency_domain_assignment.val,
        pdsch_pdu->rbStart,
        pdsch_pdu->rbSize,
        pdsch_pdu->BWPSize,
        pdsch_pdu->nrOfLayers,
        dci_payload.time_domain_assignment.val,
        dci_payload.vrb_to_prb_mapping.val,
        dci_payload.mcs,
        dci_payload.tb_scaling,
        dci_payload.ndi,
        dci_payload.rv,
        dci_payload.tpc,
        pucch ? pucch->timing_indicator : 0);

  fill_dci_pdu_rel15(&UE->sc_info,
                     current_BWP,
                     &UE->current_UL_BWP,
                     dci_pdu,
                     &dci_payload,
                     current_BWP->dci_format,
                     rnti_type,
                     sched_ctrl->search_space,
                     sched_ctrl->coreset,
                     UE->pdsch_HARQ_ACK_Codebook,
                     nr_mac->cset0_bwp_size);

  LOG_D(NR_MAC,
        "coreset params: FreqDomainResource %llx, start_symbol %d  n_symb %d\n",
        (unsigned long long)pdcch_pdu->FreqDomainResource,
        pdcch_pdu->StartSymbolIndex,
        pdcch_pdu->DurationSymbols);

  generate_dl_mac_pdu(nr_mac, UE, harq, sched_pdsch, frame, slot);
  fill_dl_tx_request(pdsch, harq->transportBlock.buf, pduindex, TBS, frame, slot);
}

void nr_schedule_ue_spec(module_id_t module_id,
                         frame_t frame,
                         slot_t slot,
                         nfapi_nr_dl_tti_request_t *DL_req,
                         nfapi_nr_tx_data_request_t *TX_req)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[module_id];
  int CC_id = 0;

  /* already mutex protected: held in gNB_dlsch_ulsch_scheduler() */
  AssertFatal(pthread_mutex_trylock(&gNB_mac->sched_lock) == EBUSY,
              "this function should be called with the scheduler mutex locked\n");

  if (!is_dl_slot(slot, &gNB_mac->frame_structure))
    return;

  NR_ServingCellConfigCommon_t *scc = gNB_mac->common_channels[CC_id].ServingCellConfigCommon;
  int bw = scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;
  gNB_mac->mac_stats.dl.total_prb_aggregate += bw;

  nfapi_nr_dl_tti_request_body_t *dl_req = &DL_req->dl_tti_request_body;
  post_process_pdsch_t pdsch = { frame, slot, dl_req, TX_req };

  /* PREPROCESSOR */
  gNB_mac->pre_processor_dl(gNB_mac, &pdsch);
}
