#include "nr_rrc_defs.h"
#include "intertask_interface.h"
#include "rrc_gNB_UE_context.h"
#include "rrc_gNB_mobility.h"
#include "ngap_gNB_ue_context.h"
#include "rrc_gNB_du.h"
#include "ran_context.h"

void rrc_get_single_ue_rnti(MessageDef *msg_p, instance_t instance);
void rrc_get_ue_context_by_ue_id(MessageDef *msg_p, instance_t instance);
void rrc_get_du_id_by_rnti(MessageDef *msg_p, instance_t instance);
void rrc_trigger_ho_f1(MessageDef *msg_p, instance_t instance);
void rrc_trigger_ho_n2(MessageDef *msg_p, instance_t instance);
void rrc_get_ngap_ue_id(MessageDef *msg_p, instance_t instance);
void rrc_check_ue_context(MessageDef *msg_p, instance_t instance);
void rrc_gnb_generate_rrcrelease(MessageDef *msg_p, instance_t instance);
void rrc_gnb_generate_rrcrelease_all(MessageDef *msg_p, instance_t instance);
