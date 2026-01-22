/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nrdc_mac_rrc_dl_handler.h"

#include "common/utils/LOG/log.h"
#include "nr_mac_gNB.h"
#include "mac_proto.h"
#include "mac_rrc_dl_handler.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "openair2/F1AP/lib/f1ap_ue_context.h"

static NR_UE_info_t *nrdc_create_new_UE(gNB_MAC_INST *mac, uint32_t cu_id, const NR_CG_ConfigInfo_t *cgci)
{
  int CC_id = 0;
  rnti_t rnti;
  bool found = nr_mac_get_new_rnti(&mac->UE_info, &rnti);
  if (!found)
    return NULL;

  f1_ue_data_t new_ue_data = {.secondary_ue = cu_id};
  bool success = du_add_f1_ue_data(rnti, &new_ue_data);
  DevAssert(success);

  NR_UE_info_t *UE = get_new_nr_ue_inst(&mac->UE_info.uid_allocator, rnti, NULL, &mac->radio_config);
  AssertFatal(UE->uid < MAX_MOBILES_PER_GNB, "cannot create UE context, UE context setup failure not implemented\n");

  NR_COMMON_channels_t *cc = &mac->common_channels[CC_id];
  const NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;
  const nr_mac_config_t *configuration = &mac->radio_config;

  NR_UE_NR_Capability_t *cap = get_ue_nr_cap_from_cg_config_info(cgci);
  int ssb_index = get_ssbidx_from_beam(mac, UE->UE_beam_index);
  NR_CellGroupConfig_t *cellGroupConfig = get_default_secondaryCellGroup(scc, cap, 1, 1, configuration, UE->uid, ssb_index);
  AssertFatal(cellGroupConfig != NULL, "out of memory\n");
  cellGroupConfig->spCellConfig->reconfigurationWithSync = get_reconfiguration_with_sync(UE->rnti, UE->uid, scc, mac->frame);
  UE->capability = cap;
  UE->local_bwp_id = 1; // nrdc_get_default_secondaryCellGroup sets 1st active BWP as 1

  // note: we don't pass the cellGroupConfig to add_new_nr_ue() because we need
  // the uid to create the CellGroupConfig (which is in the UE context created
  // by add_new_nr_ue(); it's a kind of chicken-and-egg problem), so below we
  // complete the UE context with the information that add_new_nr_ue() would
  // have added
  UE->CellGroup = cellGroupConfig;

  if (!add_new_UE_RA(mac, UE)) {
    delete_nr_ue_data(UE, &mac->UE_info.uid_allocator);
    LOG_E(NR_MAC, "UE list full while creating new UE\n");
    return NULL;
  }
  nr_mac_prepare_ra_ue(mac, UE);

  return UE;
}

void nrdc_ue_context_setup_request(const f1ap_ue_context_setup_req_t *req)
{
  LOG_D(NR_MAC, "nrdc_ue_context_setup_request called!\n");

  gNB_MAC_INST *mac = RC.nrmac[0];

  f1ap_ue_context_setup_resp_t resp = {
    .gNB_CU_ue_id = req->gNB_CU_ue_id,
  };

  const f1ap_cu_to_du_rrc_info_t *cu2du = &req->cu_to_du_rrc_info;
  NR_CG_ConfigInfo_t *cg_configinfo = get_cg_config_info(cu2du->cg_configinfo->buf, cu2du->cg_configinfo->len);

  NR_SCHED_LOCK(&mac->sched_lock);

  NR_UE_info_t *UE = nrdc_create_new_UE(mac, req->gNB_CU_ue_id, cg_configinfo);
  AssertFatal(UE, "NR-DC: cannot create a new UE, but UE Context Setup Failed not implemented yet\n");
  resp.gNB_DU_ue_id = UE->rnti;
  resp.crnti = malloc_or_fail(sizeof(*resp.crnti));
  *resp.crnti = UE->rnti;

  NR_CellGroupConfig_t *new_CellGroup = clone_CellGroupConfig(UE->CellGroup);

  if (req->drbs_len > 0)
    resp.drbs_len = handle_ue_context_drbs_setup(UE, req->drbs_len, req->drbs, &resp.drbs, new_CellGroup, &mac->rlc_config);

  NR_SCHED_UNLOCK(&mac->sched_lock);

  byte_array_t cgc = { .buf = calloc_or_fail(1,1024) };
  asn_enc_rval_t enc_rval =
      uper_encode_to_buffer(&asn_DEF_NR_CellGroupConfig, NULL, new_CellGroup, cgc.buf, 1024);
  AssertFatal(enc_rval.encoded > 0, "Could not encode CellGroup, failed element %s\n", enc_rval.failed_type->name);
  cgc.len = (enc_rval.encoded + 7) >> 3;
  resp.du_to_cu_rrc_info.cell_group_config = cgc;

  mac->mac_rrc.ue_context_setup_response(&resp);

  /* free the memory we allocated above */
  free_ue_context_setup_resp(&resp);
  ASN_STRUCT_FREE(asn_DEF_NR_CG_ConfigInfo, cg_configinfo);
}
