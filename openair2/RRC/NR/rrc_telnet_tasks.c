#include "./rrc_telnet_tasks.h"

void rrc_get_single_ue_rnti(MessageDef *msg_p, instance_t instance)
{
  if(RC.nrrrc[instance] != NULL ){
    rrc_gNB_ue_context_t *ue = NULL;
    RB_FOREACH (ue, rrc_nr_ue_tree_s, &RC.nrrrc[instance]->rrc_ue_head) {
      msg_p->ittiMsg.rrc_get_single_ue_rnti.rnti = ue->ue_context.rnti;
      msg_p->ittiMsg.rrc_get_single_ue_rnti.id = ue->ue_context.rrc_ue_id;
      msg_p->ittiMsg.rrc_get_single_ue_rnti.ue_reestablishment_counter = ue->ue_context.ue_reestablishment_counter;
      msg_p->ittiMsg.rrc_get_single_ue_rnti.ue_reconfiguration_counter = ue->ue_context.ue_reconfiguration_counter;
    }
  }
  itti_send_msg_to_task(TASK_TELNET, 0, msg_p);
}

void rrc_check_ue_context(MessageDef *msg_p, instance_t instance)
{
  if(RC.nrrrc[instance] != NULL ){
    rrc_gNB_ue_context_t *ue = rrc_gNB_get_ue_context(RC.nrrrc[instance], msg_p->ittiMsg.rrc_check_ue_context.id);
    if(!ue){
      msg_p->ittiMsg.rrc_check_ue_context.check = false;
    } else {
      msg_p->ittiMsg.rrc_check_ue_context.check = true;
    }
  }
  itti_send_msg_to_task(TASK_TELNET, 0, msg_p);
}

void rrc_get_ue_context_by_ue_id(MessageDef *msg_p, instance_t instance)
{
  if(RC.nrrrc[instance] != NULL ){
    rrc_gNB_ue_context_t *ue = NULL;
    ue = rrc_gNB_get_ue_context(RC.nrrrc[instance], msg_p->ittiMsg.rrc_get_ue_context_by_ue_id.id);
    RB_FOREACH (ue, rrc_nr_ue_tree_s, &RC.nrrrc[instance]->rrc_ue_head) {
      msg_p->ittiMsg.rrc_get_ue_context_by_ue_id.rnti = ue->ue_context.rnti;
      msg_p->ittiMsg.rrc_get_ue_context_by_ue_id.ue_reestablishment_counter = ue->ue_context.ue_reestablishment_counter;
      msg_p->ittiMsg.rrc_get_ue_context_by_ue_id.ue_reconfiguration_counter = ue->ue_context.ue_reconfiguration_counter;
    }
  }
  itti_send_msg_to_task(TASK_TELNET, 0, msg_p);
}

void rrc_get_du_id_by_rnti(MessageDef *msg_p, instance_t instance)
{
  nr_rrc_du_container_t *du = get_du_for_ue(RC.nrrrc[instance], msg_p->ittiMsg.rrc_get_du_id_by_rnti.rnti);
  msg_p->ittiMsg.rrc_get_du_id_by_rnti.du_id = du->gNB_DU_id;
  itti_send_msg_to_task(TASK_TELNET, 0, msg_p);
}

extern void nr_HO_F1_trigger_telnet(gNB_RRC_INST *rrc, uint32_t rrc_ue_id);
extern void nr_HO_N2_trigger_telnet(gNB_RRC_INST *rrc, uint32_t neighbour_pci, uint32_t rrc_ue_id);


void rrc_trigger_ho_f1(MessageDef *msg_p, instance_t instance)
{
  nr_HO_F1_trigger_telnet(RC.nrrrc[instance], msg_p->ittiMsg.rrc_trigger_ho_f1.id);
}

void rrc_trigger_ho_n2(MessageDef *msg_p, instance_t instance)
{
  nr_HO_N2_trigger_telnet(RC.nrrrc[instance], msg_p->ittiMsg.rrc_trigger_ho_n2.neighbour_pci,msg_p->ittiMsg.rrc_trigger_ho_n2.id);
}

void rrc_get_ngap_ue_id(MessageDef *msg_p, instance_t instance)
{
  ngap_gNB_ue_context_t *ngap_ue_context = ngap_get_ue_context(msg_p->ittiMsg.rrc_get_ngap_ue_id.gNB_ue_ngap_id);
  msg_p->ittiMsg.rrc_get_ngap_ue_id.amf_ue_ngap_id = ngap_ue_context->amf_ue_ngap_id;
  msg_p->ittiMsg.rrc_get_ngap_ue_id.gNB_ue_ngap_id = ngap_ue_context->gNB_ue_ngap_id;
  itti_send_msg_to_task(TASK_TELNET, 0, msg_p);
}

extern void rrc_gNB_generate_RRCRelease(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE);

void rrc_gnb_generate_rrcrelease(MessageDef *msg_p, instance_t instance)
{
  rrc_gNB_ue_context_t *ue = rrc_gNB_get_ue_context(RC.nrrrc[instance], msg_p->ittiMsg.rrc_gnb_generate_rrcrelease.ue_id);
  gNB_RRC_UE_t *UE = &ue->ue_context;
  rrc_gNB_generate_RRCRelease(RC.nrrrc[instance], UE);
}

void rrc_gnb_generate_rrcrelease_all(MessageDef *msg_p, instance_t instance)
{
  rrc_gNB_ue_context_t *ue_context_p = NULL;
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &RC.nrrrc[instance]->rrc_ue_head) {
    gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
    rrc_gNB_generate_RRCRelease(RC.nrrrc[instance], UE);
  }
}
