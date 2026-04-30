#ifndef NR_RRC_MESSAGES_TYPES_H_
#define NR_RRC_MESSAGES_TYPES_H_

typedef struct Rrc_get_single_ue_rnti_s{
  rnti_t rnti;
  ue_id_t id;
  int32_t ue_reestablishment_counter;
  int32_t ue_reconfiguration_counter;
} Rrc_get_single_ue_rnti;
#define RRC_GET_SINGLE_UE_RNTI(mSGpTR)   (mSGpTR)->ittiMsg.rrc_get_single_ue_rnti;
#define RRC_GET_UE_CONTEXT_BY_UE_ID(mSGpTR)   (mSGpTR)->ittiMsg.rrc_get_ue_context_by_ue_id;

typedef struct Rrc_get_du_id_by_rnti_s{
  rnti_t rnti;
  int du_id;
} Rrc_get_du_id_by_rnti;
#define RRC_GET_DU_ID_BY_RNTI(mSGpTR)   (mSGpTR)->ittiMsg.rrc_get_du_id_by_rnti;


typedef struct Rrc_trigger_ho_s{
  ue_id_t id;
  uint32_t neighbour_pci;
} Rrc_trigger_ho;
#define RRC_NR_HO_F1_TRIGGER(mSGpTR)   (mSGpTR)->ittiMsg.rrc_trigger_ho;
#define RRC_NR_HO_N2_TRIGGER(mSGpTR)   (mSGpTR)->ittiMsg.rrc_trigger_ho;


typedef struct Rrc_get_ngap_ue_id_s{
  int amf_ue_ngap_id;
  int gNB_ue_ngap_id;
} Rrc_get_ngap_ue_id;
#define RRC_GET_NGAP_UE_ID(mSGpTR)   (mSGpTR)->ittiMsg.rrc_ngap_ue_id;


typedef struct Rrc_check_ue_context_s{
  int id;
  bool check;
} Rrc_check_ue_context;
#define RRC_CHECK_UE_CONTEXT(mSGpTR)   (mSGpTR)->ittiMsg.rrc_check_ue_context;


typedef struct Rrc_gnb_generate_rrcrelease_s{
  ue_id_t ue_id;
} Rrc_gnb_generate_rrcrelease;
#define RRC_GNB_GENERATE_RRCRELEASE(mSGpTR)   (mSGpTR)->ittiMsg.rrc_gnb_generate_rrcrelease;


typedef struct Rrc_gnb_generate_rrcrelease_all_s{
  Rrc_gnb_generate_rrcrelease rrc_gnb_generate_rrcreleases[64];
} Rrc_gnb_generate_rrcrelease_all;
#define RRC_GNB_GENERATE_RRCRELEASE_ALL(mSGpTR)   (mSGpTR)->ittiMsg.rrc_gnb_generate_rrcrelease_all;


#endif /* NR_RRC_MESSAGES_TYPES_H_ */
