/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "rrc_gNB_nrdc.h"

#include "rrc_gNB_UE_context.h"
#include "rrc_gNB_du.h"
#include "rrc_gNB_measurements.h"
#include "LAYER2/NR_MAC_COMMON/nr_mac.h"
#include "NR_UE-CapabilityRequestFilterNR.h"
#include "NR_UECapabilityEnquiry-v1560-IEs.h"

typedef enum {
  NRDC_NONE,
  ACTIVATE_NRDC_WAIT_FOR_CAPABILITIES
} nrdc_state_t;

typedef struct {
  nrdc_state_t state;
  int mcg_band;
  int scg_band;
  int xid;
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
}
