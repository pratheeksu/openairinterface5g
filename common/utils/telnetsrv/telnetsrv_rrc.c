/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "intertask_interface.h"

#include "openair2/RRC/NR/rrc_gNB_UE_context.h"

#define TELNETSERVERCODE
#include "telnetsrv.h"

#define ERROR_MSG_RET(mSG, aRGS...) do { prnt(mSG, ##aRGS); return 1; } while (0)

/**
 * Module brief:
 * This module is used to add RRCRelease commands to the telnet server in the
 * absence of full support for E2SM RAN Control (RC).
 * This provides similar functionality to the ORAN.WG3.E2SM-RC-R003-v05.00
 * 8.4.5.4 RRC Connection Release Control which is initiated by the RIC.
 *
 * Implementation notes:
 * We refer to the method call rrc_gNB_generate_RRCRelease at rrc_gNB_NGAP.c
 * during rrc_gNB_process_NGAP_UE_CONTEXT_RELEASE_COMMAND message generation.
 *
 * Building the telnetsrv and module:
 * ./build_oai --build-lib telnetsrv
 *
 * Loading the module:
 * sudo ./nr-softmodem -E --rfsim --log_config.global_log_options level,nocolor,time -O ~/gnb.sa.band78.106prb.rfsim.conf --telnetsrv --telnetsrv.shrmod rrc
*/

static int get_single_ue_id(void)
{
  MessageDef *msg_p = itti_alloc_new_message (TASK_RRC_GNB, 0, RRC_GET_SINGLE_UE_RNTI);
  itti_send_msg_to_task(TASK_RRC_GNB, 0, msg_p);
  itti_receive_msg(TASK_TELNET, &msg_p);
  return msg_p->ittiMsg.rrc_get_single_ue_rnti.rnti ? msg_p->ittiMsg.rrc_get_single_ue_rnti.rnti : -1;
}

/**
 * @brief Trigger RRC Release for a specific UE
 * @param buf: RRC UE ID
 * @param debug: Debug flag
 * @param prnt: Print function
 * @return 0 on success, -1 on failure
*/
int rrc_gNB_trigger_release(char *buf, int debug, telnet_printfunc_t prnt)
{
  UNUSED(debug);
  ue_id_t ue_id = -1;

  if (!buf) {
    ue_id = get_single_ue_id();
    if (ue_id < 1) {
      prnt("No UE found!\n");
      ERROR_MSG_RET("No UE found!\n");
    }
  } else {
    ue_id = strtol(buf, NULL, 10);
    if (ue_id < 1 || ue_id >= 0xfffffe) {
      prnt("UE ID needs to be [1,0xfffffe]\n");
      ERROR_MSG_RET("UE ID needs to be [1,0xfffffe]\n");
    }
  }

  /* get RRC and UE */

  MessageDef *msg_p = itti_alloc_new_message (TASK_RRC_GNB, 0, RRC_GET_UE_CONTEXT_BY_UE_ID);
  itti_send_msg_to_task(TASK_RRC_GNB, 0, msg_p);
  itti_receive_msg(TASK_TELNET, &msg_p);
  Rrc_get_single_ue_rnti ue = msg_p->ittiMsg.rrc_get_single_ue_rnti;
  if (!ue.rnti) {
    prnt("Could not find UE context associated with UE ID %lu\n", ue_id);
    LOG_E(RRC, "Could not find UE context associated with UE ID %lu\n", ue_id);
    return -1;
  }

  msg_p = itti_alloc_new_message (TASK_RRC_GNB, 0, RRC_GNB_GENERATE_RRCRELEASE);
  msg_p->ittiMsg.rrc_gnb_generate_rrcrelease.ue_id = ue.id;
  itti_send_msg_to_task(TASK_RRC_GNB, 0, msg_p);

  prnt("RRC Release triggered for UE %u\n", ue_id);

  return 0;
}

/**
 * @brief Trigger RRC Release for all UEs
*/
int rrc_gNB_trigger_release_all(char *buf, int debug, telnet_printfunc_t prnt)
{
  UNUSED(debug);
  UNUSED(buf);
  MessageDef *msg_p = itti_alloc_new_message (TASK_RRC_GNB, 0, RRC_GNB_GENERATE_RRCRELEASE_ALL);
  itti_send_msg_to_task(TASK_RRC_GNB, 0, msg_p);
  itti_receive_msg(TASK_TELNET, &msg_p);
  for(int i=0; i<sizeof(msg_p->ittiMsg.rrc_gnb_generate_rrcrelease_all.rrc_gnb_generate_rrcreleases);i++){
    prnt("RRC Release triggered for UE %u\n", msg_p->ittiMsg.rrc_gnb_generate_rrcrelease_all.rrc_gnb_generate_rrcreleases[i].ue_id);
  }
  return 0;
}

static telnetshell_cmddef_t rrc_cmds[] = {
  {"release_rrc", "[rrc_ue_id(int,opt)]", rrc_gNB_trigger_release},
  {"release_rrc_all", "", rrc_gNB_trigger_release_all},
  {"", "", NULL},
};

static telnetshell_vardef_t rrc_vars[] = {
  {"", 0, 0, NULL}
};

void add_rrc_cmds(void) {
  add_telnetcmd("rrc", rrc_vars, rrc_cmds);
}
