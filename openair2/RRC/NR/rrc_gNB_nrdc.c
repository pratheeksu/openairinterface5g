/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "rrc_gNB_nrdc.h"

#include "rrc_gNB_UE_context.h"
#include "rrc_gNB_du.h"
#include "rrc_cell_management.h"
#include "rrc_gNB_measurements.h"
#include "LAYER2/NR_MAC_COMMON/nr_mac.h"
#include "NR_UE-CapabilityRequestFilterNR.h"
#include "NR_UECapabilityEnquiry-v1560-IEs.h"
#include "common/utils/time_manager/time_timer.h"

typedef enum {
  NRDC_NONE,
  ACTIVATE_NRDC_WAIT_FOR_CAPABILITIES,
  ACTIVATE_NRDC_WAIT_FOR_A4_RECONFIGURATION_COMPLETE,
  ACTIVATE_NRDC_WAIT_FOR_SCG_MEASUREMENT,
  NRDC_ACTIVE
} nrdc_state_t;

typedef struct {
  nrdc_state_t state;
  int mcg_band;
  int scg_band;
  int xid;
  void *timer;
  int meas_object_id;
  int report_config_id;
  int measurement_id;
} nrdc_ue_state_t;

static int generate_ue_capability_enquiry(uint8_t *out, int outsize, int xid, int mcg_band, int scg_band)
{
  /* capability request filter */
  NR_UE_CapabilityRequestFilterNR_t filter = {
    .frequencyBandListFilter = &(struct NR_FreqBandList) {
      .list = {
        .array = (struct NR_FreqBandInformation *[]) {
          &(struct NR_FreqBandInformation) {
            .present = NR_FreqBandInformation_PR_bandInformationNR,
            .choice = {
              .bandInformationNR = &(struct NR_FreqBandInformationNR) {
                .bandNR = (NR_FreqBandIndicatorNR_t) mcg_band
              }
            }
          },
          &(struct NR_FreqBandInformation) {
            .present = NR_FreqBandInformation_PR_bandInformationNR,
            .choice = {
              .bandInformationNR = &(struct NR_FreqBandInformationNR) {
                .bandNR = (NR_FreqBandIndicatorNR_t) scg_band
              }
            }
          }
        },
        .count = 2
      }
    }
  };

  uint8_t filter_buf[512];
  int filter_buf_size = sizeof(filter_buf);
  asn_enc_rval_t size = uper_encode_to_buffer(&asn_DEF_NR_UE_CapabilityRequestFilterNR, NULL, &filter, filter_buf, filter_buf_size);
  AssertFatal(size.encoded > 0, "failed to encode FreqBandInformationNR\n");
  filter_buf_size = (size.encoded + 7) / 8;

  /* capability enquiry extension */
  NR_UECapabilityEnquiry_v1560_IEs_t ext = {
    .capabilityRequestFilterCommon = &(struct NR_UE_CapabilityRequestFilterCommon) {
      .mrdc_Request = &(struct NR_UE_CapabilityRequestFilterCommon__mrdc_Request) {
        .includeNR_DC = &(long) { NR_UE_CapabilityRequestFilterCommon__mrdc_Request__includeNR_DC_true }
      },
      .ext2 = &(struct NR_UE_CapabilityRequestFilterCommon__ext2) {
        .requestedCellGrouping_r16 = &(struct NR_UE_CapabilityRequestFilterCommon__ext2__requestedCellGrouping_r16) {
          .list = {
            .array = (struct NR_CellGrouping_r16 *[]) {
              &(struct NR_CellGrouping_r16) {
                .mcg_r16 = {
                  .list = {
                    .array = (NR_FreqBandIndicatorNR_t *[]) {
                      &(NR_FreqBandIndicatorNR_t) { mcg_band }
                    },
                    .count = 1
                  }
                },
                .scg_r16 = {
                  .list = {
                    .array = (NR_FreqBandIndicatorNR_t *[]) {
                      &(NR_FreqBandIndicatorNR_t) { scg_band }
                    },
                    .count = 1
                  }
                },
                /* not sure of this sync/async mode, see 38.133 chapter 7
                 * and maybe 38.321, 38.214, 38.401 (chapter 9, synchronization)
                 * for TAG where timing matters
                 */
                .mode_r16 = NR_CellGrouping_r16__mode_r16_async
              }
            },
            .count = 1
          }
        }
      }
    }
  };

  uint8_t ext_buf[512];
  int ext_buf_size = sizeof(ext_buf);
  size = uper_encode_to_buffer(&asn_DEF_NR_UECapabilityEnquiry_v1560_IEs, NULL, &ext, ext_buf, ext_buf_size);
  AssertFatal(size.encoded > 0, "failed to encode FreqBandInformationNR\n");
  ext_buf_size = (size.encoded + 7) / 8;

  /* capability enquiry */
  NR_DL_DCCH_Message_t m = {
    .message = {
      .present = NR_DL_DCCH_MessageType_PR_c1,
      .choice = {
        .c1 = &(struct NR_DL_DCCH_MessageType__c1) {
          .present = NR_DL_DCCH_MessageType__c1_PR_ueCapabilityEnquiry,
          .choice = {
            .ueCapabilityEnquiry = &(struct NR_UECapabilityEnquiry) {
              .rrc_TransactionIdentifier = xid,
              .criticalExtensions = {
                .present = NR_UECapabilityEnquiry__criticalExtensions_PR_ueCapabilityEnquiry,
                .choice = {
                  .ueCapabilityEnquiry = &(struct NR_UECapabilityEnquiry_IEs) {
                    .ue_CapabilityRAT_RequestList = {
                      .list = {
                        .array = (struct NR_UE_CapabilityRAT_Request *[]) {
                          &(struct NR_UE_CapabilityRAT_Request) {
                            .rat_Type = NR_RAT_Type_nr,
                            .capabilityRequestFilter = &(OCTET_STRING_t) {
                              .buf = filter_buf,
                              .size = filter_buf_size
                            }
                          }
                        },
                        .count = 1
                      }
                    },
                    .ue_CapabilityEnquiryExt = &(OCTET_STRING_t) {
                      .buf = ext_buf,
                      .size = ext_buf_size
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  };

  size = uper_encode_to_buffer(&asn_DEF_NR_DL_DCCH_Message, NULL, &m, out, outsize);
  AssertFatal(size.encoded > 0, "failed to encode UECapabilityEnquiry\n");

  return (size.encoded + 7) / 8;
}

static int generate_a4_measurement(gNB_RRC_UE_t *ue,
                                   uint8_t *out,
                                   int outsize,
                                   int xid,
                                   int scg_band,
                                   int scg_ssb_arfcn,
                                   int scg_ssb_subcarrier_spacing,
                                   NR_SSB_MTC_t *du_ssb_mtc)
{
  nrdc_ue_state_t *nrdc = ue->nrdc;
  nrdc->meas_object_id = allocate_measurement_object_id(ue);
  nrdc->report_config_id = allocate_report_config_id(ue);
  nrdc->measurement_id = allocate_measurement_id(ue);

  /* This is needed.
   * When using du_ssb_mtc of an FR2 cell, the UE does not report
   * measurements.
   * To be fixed/understood better.
   */
  NR_SSB_MTC_t mtc = {
    .periodicityAndOffset = {
      .present = NR_SSB_MTC__periodicityAndOffset_PR_sf20,
      .choice = {
        .sf20 = 0
      }
    },
    .duration = NR_SSB_MTC__duration_sf5
  };
  du_ssb_mtc = &mtc;

  NR_DL_DCCH_Message_t m = {
    .message = {
      .present = NR_DL_DCCH_MessageType_PR_c1,
      .choice = {
        .c1 = &(struct NR_DL_DCCH_MessageType__c1) {
          .present = NR_DL_DCCH_MessageType__c1_PR_rrcReconfiguration,
          .choice = {
            .rrcReconfiguration = &(struct NR_RRCReconfiguration) {
              .rrc_TransactionIdentifier = xid,
              .criticalExtensions = {
                .present = NR_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration,
                .choice = {
                  .rrcReconfiguration = &(struct NR_RRCReconfiguration_IEs){
                    .measConfig = &(struct NR_MeasConfig) {
                      .measObjectToAddModList = &(struct NR_MeasObjectToAddModList) {
                        .list = {
                          .array = (struct NR_MeasObjectToAddMod *[]) {
                            &(struct NR_MeasObjectToAddMod) {
                              .measObjectId = nrdc->meas_object_id,
                              .measObject = {
                                .present = NR_MeasObjectToAddMod__measObject_PR_measObjectNR,
                                .choice = {
                                  .measObjectNR = &(struct NR_MeasObjectNR) {
                                    .ssbFrequency = &(NR_ARFCN_ValueNR_t) { scg_ssb_arfcn },
                                    .ssbSubcarrierSpacing = &(NR_SubcarrierSpacing_t) { scg_ssb_subcarrier_spacing },
                                    .smtc1 = du_ssb_mtc,
                                    .referenceSignalConfig = {
                                      .ssb_ConfigMobility = &(struct NR_SSB_ConfigMobility) {
                                        //.deriveSSB_IndexFromCell = false                   /* hardcoded */
                                        .deriveSSB_IndexFromCell = true                    /* hardcoded */
                                      }
                                    },
                                    .absThreshSS_BlocksConsolidation = &(struct NR_ThresholdNR) {
                                      .thresholdRSRP = &(NR_RSRP_Range_t) { 36 }           /* hardcoded */
                                    },
                                    .nrofSS_BlocksToAverage = &(long) { 8 },               /* hardcoded */
                                    .quantityConfigIndex = 1,                              /* hardcoded, todo: check this value */
                                    .ext1 = &(struct NR_MeasObjectNR__ext1) {
                                      .freqBandIndicatorNR = &(NR_FreqBandIndicatorNR_t) { scg_band }
                                    }
                                  }
                                }
                              }
                            }
                          },
                          .count = 1
                        }
                      },
                      .reportConfigToAddModList = &(struct NR_ReportConfigToAddModList) {
                        .list = {
                          .array = (struct NR_ReportConfigToAddMod *[]) {
                            &(struct NR_ReportConfigToAddMod) {
                              .reportConfigId = nrdc->report_config_id,
                              .reportConfig = {
                                .present = NR_ReportConfigToAddMod__reportConfig_PR_reportConfigNR,
                                .choice = {
                                  .reportConfigNR = &(struct NR_ReportConfigNR) {
                                    .reportType = {
                                      .present = NR_ReportConfigNR__reportType_PR_eventTriggered,
                                      .choice = {
                                        .eventTriggered = &(struct NR_EventTriggerConfig) {
                                          .eventId = {
                                            .present = NR_EventTriggerConfig__eventId_PR_eventA4,
                                            .choice = {
                                              .eventA4 = &(struct NR_EventTriggerConfig__eventId__eventA4) {
                                                .a4_Threshold = {
                                                  .present = NR_MeasTriggerQuantity_PR_rsrp,
                                                  .choice = {
                                                    .rsrp = 20                             /* hardcoded */
                                                  }
                                                },
                                                .reportOnLeave = false,                    /* hardcoded */
                                                .hysteresis = 0,                           /* hardcoded */
                                                .timeToTrigger = NR_TimeToTrigger_ms40,    /* hardcoded */
                                                .useAllowedCellList = false                /* todo: check this */
                                              }
                                            }
                                          },
                                          .rsType = NR_NR_RS_Type_ssb,                     /* hardcoded */
                                          .reportInterval = NR_ReportInterval_ms120,       /* hardcoded */
                                          .reportAmount = NR_EventTriggerConfig__reportAmount_r2,      /* todo: choose a proper value, maybe 1 would be enough? */
                                          .reportQuantityCell = {
                                            .rsrp = true,                                  /* hardcoded */
                                            .rsrq = true,                                  /* hardcoded */
                                            .sinr = true                                   /* hardcoded */
                                          },
                                          .maxReportCells = 4,                             /* hardcoded */
                                          .includeBeamMeasurements = false                 /* hardcoded */
                                        }
                                      }
                                    }
                                  }
                                }
                              }
                            }
                          },
                          .count = 1
                        }
                      },
                      .measIdToAddModList = &(struct NR_MeasIdToAddModList) {
                        .list = {
                          .array = (struct NR_MeasIdToAddMod *[]) {
                            &(struct NR_MeasIdToAddMod) {
                              .measId = nrdc->measurement_id,
                              .measObjectId = nrdc->meas_object_id,
                              .reportConfigId = nrdc->report_config_id
                            }
                          },
                          .count = 1
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  };
  asn_enc_rval_t size = uper_encode_to_buffer(&asn_DEF_NR_DL_DCCH_Message, NULL, &m, out, outsize);
  AssertFatal(size.encoded > 0, "failed to encode A4 measurement\n");

  return (size.encoded + 7) / 8;
}

/* called from timer thread, so simply send a "nrdc timeout" ITTI message to
 * RRC that will do the real work
 */
static void timeout(void **p, uint64_t *v)
{
  gNB_RRC_INST *rrc = p[0];
  uint64_t ue_id = v[0];
  int xid = v[1];
  nrdc_state_t state = v[2];

  MessageDef *message_p = itti_alloc_new_message(TASK_RRC_GNB, rrc->module_id, NR_RRC_NRDC_TIMEOUT);
  nr_rrc_nrdc_timeout_t *m = &NR_RRC_NRDC_TIMEOUT(message_p);
  memset(m, 0, sizeof(*m));
  m->ue_id = ue_id;
  m->xid = xid;
  m->state = state;

  itti_send_msg_to_task(TASK_RRC_GNB, rrc->module_id, message_p);
}

/* This function checks that the CU has DUs connected that correspond
 * to a configured NR-DC combination.
 * For the first one that is found, the UE capabilities for NR-DC are
 * requested.
 */
void rrc_gnb_nrdc_start(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue)
{
  /* if already in NR-DC, do nothing */
  if (ue->nrdc)
    return;

  /* look for an NR-DC combination */
  int mcg_band = 77;
  int scg_band = 257;

  /* a combination is found, start NR-DC, ask for UE capabilities */
  /* allocate the NR-DC state data in the UE */
  nrdc_ue_state_t *nrdc = calloc_or_fail(1, sizeof(*nrdc));
  ue->nrdc = nrdc;

  nrdc->state = ACTIVATE_NRDC_WAIT_FOR_CAPABILITIES;

  nrdc->mcg_band = mcg_band;
  nrdc->scg_band = scg_band;

  int xid = rrc_gNB_get_next_transaction_identifier(rrc->module_id);
  nrdc->xid = xid;
  ue->xids[xid] = RRC_F1_NRDC_IN_PROGRESS;

  uint8_t ue_cap[1024];
  int size = generate_ue_capability_enquiry(ue_cap, sizeof(ue_cap), xid, mcg_band, scg_band);

  /* send it to the UE */
  uint32_t msg_id = NR_DL_DCCH_MessageType__c1_PR_ueCapabilityEnquiry;
  nr_rrc_transfer_protected_rrc_message(rrc, ue, DL_SCH_LCID_DCCH, msg_id, ue_cap, size);

  /* start a timer to react after some time (2s, arbitrary value) if the UE does not reply */
  nrdc->timer = tick_timeout_start(2000, timeout,
                                  (void *[]){ rrc }, 1,
                                  (uint64_t []){ ue->rrc_ue_id, xid, nrdc->state }, 3);
}

void rrc_gnb_nrdc_ue_capabilities_received(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue,  const NR_UECapabilityInformation_t *ue_cap)
{
  LOG_D(NR_RRC, "rrc_gnb_nrdc_ue_capabilities_received called!!\n");

  nrdc_ue_state_t *nrdc = ue->nrdc;
  if (!nrdc) {
    LOG_W(NR_RRC, "no NR-DC context found for ue %d\n", ue->rrc_ue_id);
    return;
  }

  if (nrdc->state != ACTIVATE_NRDC_WAIT_FOR_CAPABILITIES) {
    LOG_W(NR_RRC, "ignore unexpected NR-DC capabilities received for ue %d\n", ue->rrc_ue_id);
    return;
  }

  /* stop the timer */
  tick_timeout_stop(nrdc->timer);
  nrdc->timer = 0;

  ue->xids[nrdc->xid] = RRC_ACTION_NONE;
  nrdc->xid = -1;

  /* check capabilities */
  /* todo */

  /* capabilities accept the configured NR-DC combination */
  nrdc->state = ACTIVATE_NRDC_WAIT_FOR_A4_RECONFIGURATION_COMPLETE;

  nr_rrc_cell_container_t *cell = get_cell_by_band(&rrc->cells, nrdc->scg_band);
  if (!cell) {
    /* no cell found, cancel the process */
    LOG_E(NR_RRC, "no cell found for SCG band %d, cancelling NR-DC activation procedure for UE %d\n", nrdc->scg_band, ue->rrc_ue_id);
    free(ue->nrdc);
    ue->nrdc = 0;
    return;
  }
  NR_MeasTimingList_t *mtlist = cell->mtc->criticalExtensions.choice.c1->choice.measTimingConf->measTiming;
  NR_SSB_MTC_t *ssb_mtc = &mtlist->list.array[0]->frequencyAndTiming->ssb_MeasurementTimingConfiguration;

  /* configure A4 for the SCG SpCell */
  int xid = rrc_gNB_get_next_transaction_identifier(rrc->module_id);
  nrdc->xid = xid;
  ue->xids[xid] = RRC_F1_NRDC_IN_PROGRESS;

  uint8_t a4_buf[1024];
  int scs = cell->info.mode == NR_MODE_TDD ? cell->info.tdd.dlul.scs : cell->info.fdd.dl.scs;
  int size = generate_a4_measurement(ue,
                                     a4_buf,
                                     sizeof(a4_buf),
                                     xid,
                                     nrdc->scg_band,
                                     get_ssb_arfcn(cell),
                                     scs,
                                     ssb_mtc);

  /* send it to the UE */
  uint32_t msg_id = NR_DL_DCCH_MessageType__c1_PR_rrcReconfiguration;
  nr_rrc_transfer_protected_rrc_message(rrc, ue, DL_SCH_LCID_DCCH, msg_id, a4_buf, size);

  /* start a timer to react after some time (2s, arbitrary value) if the UE does not reply */
  nrdc->timer = tick_timeout_start(2000, timeout,
                                   (void *[]){ rrc }, 1,
                                   (uint64_t []){ ue->rrc_ue_id, xid, nrdc->state }, 3);
}

void rrc_gnb_nrdc_rrc_reconfiguration_complete_received(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, int xid)
{
  LOG_D(NR_RRC, "rrc_gnb_nrdc_rrc_reconfiguration_complete_received called!!\n");

  nrdc_ue_state_t *nrdc = ue->nrdc;
  if (!nrdc) {
    LOG_W(NR_RRC, "no NR-DC context found for ue %d\n", ue->rrc_ue_id);
    return;
  }

  if (nrdc->state != ACTIVATE_NRDC_WAIT_FOR_A4_RECONFIGURATION_COMPLETE) {
    LOG_W(NR_RRC, "ignore unexpected NR-DC RRC Reconfiguration received for ue %d\n", ue->rrc_ue_id);
    return;
  }

  if (xid != nrdc->xid) {
    LOG_W(NR_RRC, "ignore wrong NR-DC procedure for ue %d\n", ue->rrc_ue_id);
    return;
  }

  /* stop the timer */
  tick_timeout_stop(nrdc->timer);
  nrdc->timer = 0;

  ue->xids[nrdc->xid] = RRC_ACTION_NONE;
  nrdc->xid = -1;

  /* measurements correctly configured for NR-DC combination
   * do nothing until we receive a measurement
   * (or until the UE is removed from the system)
   */
  nrdc->state = ACTIVATE_NRDC_WAIT_FOR_SCG_MEASUREMENT;
}

void rrc_gnb_nrdc_measurement_received(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, NR_MeasurementReport_t *meas)
{
  LOG_E(NR_RRC, "rrc_gnb_nrdc_measurement_received called!!\n");
}

void rrc_gnb_nrdc_timeout(gNB_RRC_INST *rrc, nr_rrc_nrdc_timeout_t *timeout)
{
  rrc_gNB_ue_context_t *ue_context = rrc_gNB_get_ue_context(rrc, timeout->ue_id);
  if (!ue_context) {
    LOG_W(NR_RRC, "UE %"PRIu64" not found, discard NR-DC timeout\n", timeout->ue_id);
    return;
  }
  gNB_RRC_UE_t *ue = &ue_context->ue_context;
  int state = timeout->state;
  int xid = timeout->xid;

  nrdc_ue_state_t *nrdc = ue->nrdc;
  if (!nrdc) {
    LOG_W(NR_RRC, "no NR-DC context found for ue %d, discard timeout\n", ue->rrc_ue_id);
    return;
  }
  if (nrdc->state != state) {
    LOG_W(NR_RRC, "bad state %d for NR-DC context of ue %d, discard timeout\n", state, ue->rrc_ue_id);
    return;
  }
  if (nrdc->xid != xid) {
    LOG_W(NR_RRC, "bad xid %d for NR-DC context of ue %d, discard timeout\n", xid, ue->rrc_ue_id);
    return;
  }

  /* stop the timer (if any) */
  if (nrdc->timer)
    tick_timeout_stop(nrdc->timer);
  nrdc->timer = 0;

  /* clear the current action for this xid (if any) */
  if (xid != -1) {
    ue->xids[xid] = RRC_ACTION_NONE;
  }

  /* cancel the NR-DC activation procedure */
  LOG_E(NR_RRC, "timeout, cancelling NR-DC activation procedure for UE %d\n", ue->rrc_ue_id);
  free(ue->nrdc);
  ue->nrdc = 0;
}

int get_scg_measurement_id(gNB_RRC_UE_t *ue)
{
  if (!ue->nrdc)
    return -1;
  nrdc_ue_state_t *nrdc = ue->nrdc;
  return nrdc->measurement_id;
}
