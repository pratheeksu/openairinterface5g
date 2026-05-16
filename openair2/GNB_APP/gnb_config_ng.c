/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "gnb_config_ng.h"
#include <stdint.h>
#include "assertions.h"
#include "common/ran_context.h"
#include "openair2/GNB_APP/gnb_config_common.h"
#include "common/config/config_paramdesc.h"
#include "sctp_default_values.h"
#include "gnb_paramdef.h"
#include "common/config/config_userapi.h"
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

int RCconfig_NR_NG(MessageDef *msg_p, uint32_t i)
{
  int j, k = 0;
  int gnb_id;

  char *gnb_ipv4_address_for_NGU = NULL;
  uint32_t gnb_port_for_NGU = 0;
  char *gnb_ipv4_address_for_S1U = NULL;
  uint32_t gnb_port_for_S1U = 0;

  GET_PARAMS(GNBSParams, GNBSPARAMS_DESC, NULL);
  AssertFatal(i < GNBSParams[GNB_ACTIVE_GNBS_IDX].numelt,
              "Failed to parse config file %s, %uth attribute %s \n",
              RC.config_file_name,
              i,
              GNB_CONFIG_STRING_ACTIVE_GNBS);

  if (GNBSParams[GNB_ACTIVE_GNBS_IDX].numelt > 0) {
    // Output a list of all gNBs.
    GET_PARAMS_LIST(GNBParamList, GNBParams, GNBPARAMS_DESC, GNB_CONFIG_STRING_GNB_LIST, NULL);
    if (GNBParamList.numelt > 0) {
      for (k = 0; k < GNBParamList.numelt; k++) {
        if (GNBParamList.paramarray[k][GNB_GNB_ID_IDX].uptr == NULL) {
          // Calculate a default gNB ID
          if (IS_SA_MODE(get_softmodem_params())) {
            uint32_t hash;

            hash = ngap_generate_gNB_id();
            gnb_id = k + (hash & 0xFFFFFF8);
          } else {
            gnb_id = k;
          }
        } else {
          gnb_id = *(GNBParamList.paramarray[k][GNB_GNB_ID_IDX].uptr);
        }

        // search if in active list
        for (j = 0; j < GNBSParams[GNB_ACTIVE_GNBS_IDX].numelt; j++) {
          if (strcmp(GNBSParams[GNB_ACTIVE_GNBS_IDX].strlistptr[j], *(GNBParamList.paramarray[k][GNB_GNB_NAME_IDX].strptr)) == 0) {
            char aprefix[MAX_OPTNAME_SIZE * 2 + 8];
            NGAP_REGISTER_GNB_REQ(msg_p).gNB_id = gnb_id;

            if (strcmp(*(GNBParamList.paramarray[k][GNB_CELL_TYPE_IDX].strptr), "CELL_MACRO_GNB") == 0) {
              NGAP_REGISTER_GNB_REQ(msg_p).cell_type = CELL_MACRO_GNB;
            } else if (strcmp(*(GNBParamList.paramarray[k][GNB_CELL_TYPE_IDX].strptr), "CELL_HOME_GNB") == 0) {
              NGAP_REGISTER_GNB_REQ(msg_p).cell_type = CELL_HOME_ENB;
            } else {
              AssertFatal(0,
                          "Failed to parse gNB configuration file %s, gnb %u unknown value \"%s\" for cell_type choice: "
                          "CELL_MACRO_GNB or CELL_HOME_GNB !\n",
                          RC.config_file_name,
                          i,
                          *(GNBParamList.paramarray[k][GNB_CELL_TYPE_IDX].strptr));
            }

            NGAP_REGISTER_GNB_REQ(msg_p).gNB_name = strdup(*(GNBParamList.paramarray[k][GNB_GNB_NAME_IDX].strptr));
            NGAP_REGISTER_GNB_REQ(msg_p).tac = *GNBParamList.paramarray[k][GNB_TRACKING_AREA_CODE_IDX].uptr;
            AssertFatal(!GNBParamList.paramarray[k][GNB_MOBILE_COUNTRY_CODE_IDX_OLD].strptr
                            && !GNBParamList.paramarray[k][GNB_MOBILE_NETWORK_CODE_IDX_OLD].strptr,
                        "It seems that you use an old configuration file. Please change the existing\n"
                        "    tracking_area_code  =  \"1\";\n"
                        "    mobile_country_code =  \"208\";\n"
                        "    mobile_network_code =  \"93\";\n"
                        "to\n"
                        "    tracking_area_code  =  1; // no string!!\n"
                        "    plmn_list = ( { mcc = 208; mnc = 93; mnc_length = 2; } )\n");
            // PLMN
            plmn_id_t p[PLMN_LIST_MAX_SIZE] = {0};
            NGAP_REGISTER_GNB_REQ(msg_p).num_plmn = set_plmn_config(p, 0);
            for (int l = 0; l < NGAP_REGISTER_GNB_REQ(msg_p).num_plmn; ++l) {
              NGAP_REGISTER_GNB_REQ(msg_p).plmn[l].plmn = p[l];
              // SNSSAI
              NGAP_REGISTER_GNB_REQ(msg_p).plmn[l].num_nssai =
                  set_snssai_config(NGAP_REGISTER_GNB_REQ(msg_p).plmn[l].s_nssai, 8, k, l);
            }
            NGAP_REGISTER_GNB_REQ(msg_p).default_drx = 0;
            // NG
            snprintf(aprefix, sizeof(aprefix), "%s.[%i]", GNB_CONFIG_STRING_GNB_LIST, k);
            GET_PARAMS_LIST(NGParamList, NGParams, GNBNGPARAMS_DESC, GNB_CONFIG_STRING_AMF_IP_ADDRESS, aprefix);
            NGAP_REGISTER_GNB_REQ(msg_p).nb_amf = 0;

            for (int l = 0; l < NGParamList.numelt; l++) {
              NGAP_REGISTER_GNB_REQ(msg_p).nb_amf += 1;
              struct in_addr tmp;
              int ret = inet_pton(AF_INET, *(NGParamList.paramarray[l][GNB_AMF_IPV4_ADDRESS_IDX].strptr), &tmp);
              if (ret == 0) {
                const char *port_str = "2152";
                struct addrinfo hints, *res;
                memset(&hints, 0x00, sizeof(hints));
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                int h_addr;
                char buf[INET_ADDRSTRLEN];
                LOG_I(GNB_APP, "Domain Name detected: %s\n", *(NGParamList.paramarray[l][GNB_AMF_IPV4_ADDRESS_IDX].strptr));
                h_addr = getaddrinfo(*(NGParamList.paramarray[l][GNB_AMF_IPV4_ADDRESS_IDX].strptr), port_str, &hints, &res);
                if (h_addr != 0) {
                  LOG_W(GNB_APP, "Invalid AMF address\n");
                  exit(-1);
                }
                struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &addr->sin_addr, buf, INET_ADDRSTRLEN);
                strcpy(NGAP_REGISTER_GNB_REQ(msg_p).amf_ip_address[l].ipv4_address, buf);
                LOG_I(GNB_APP, "Resolved IPv4 address: %s\n", buf);
                freeaddrinfo(res);
              } else {
                strcpy(NGAP_REGISTER_GNB_REQ(msg_p).amf_ip_address[l].ipv4_address,
                       *(NGParamList.paramarray[l][GNB_AMF_IPV4_ADDRESS_IDX].strptr));
              }

              NGAP_REGISTER_GNB_REQ(msg_p).amf_ip_address[l].ipv4 = 1;
              NGAP_REGISTER_GNB_REQ(msg_p).amf_ip_address[l].ipv6 = 0;
              /* if no broadcasst_plmn array is defined, fill default values */
              if (NGAP_REGISTER_GNB_REQ(msg_p).broadcast_plmn_num[l] == 0) {
                NGAP_REGISTER_GNB_REQ(msg_p).broadcast_plmn_num[l] = NGAP_REGISTER_GNB_REQ(msg_p).num_plmn;
                for (int el = 0; el < NGAP_REGISTER_GNB_REQ(msg_p).num_plmn; ++el)
                  NGAP_REGISTER_GNB_REQ(msg_p).broadcast_plmn_index[l][el] = el;
              }
            }

            // SCTP SETTING
            NGAP_REGISTER_GNB_REQ(msg_p).sctp_out_streams = SCTP_OUT_STREAMS;
            NGAP_REGISTER_GNB_REQ(msg_p).sctp_in_streams = SCTP_IN_STREAMS;
            if (IS_SA_MODE(get_softmodem_params())) {
              snprintf(aprefix, sizeof(aprefix), "%s.[%d].%s", GNB_CONFIG_STRING_GNB_LIST, k, GNB_CONFIG_STRING_SCTP_CONFIG);
              GET_PARAMS(SCTPParams, GNBSCTPPARAMS_DESC, aprefix);
              NGAP_REGISTER_GNB_REQ(msg_p).sctp_in_streams = (uint16_t) * (SCTPParams[GNB_SCTP_INSTREAMS_IDX].uptr);
              NGAP_REGISTER_GNB_REQ(msg_p).sctp_out_streams = (uint16_t) * (SCTPParams[GNB_SCTP_OUTSTREAMS_IDX].uptr);
            }

            // NETWORK_INTERFACES
            snprintf(aprefix,
                     sizeof(aprefix),
                     "%s.[%d].%s",
                     GNB_CONFIG_STRING_GNB_LIST,
                     k,
                     GNB_CONFIG_STRING_NETWORK_INTERFACES_CONFIG);
            GET_PARAMS(NETParams, GNBNETPARAMS_DESC, aprefix);
            if (NETParams[GNB_IPV4_ADDRESS_FOR_NG_AMF_IDX].strptr != NULL) {
              char *cidr = *(NETParams[GNB_IPV4_ADDRESS_FOR_NG_AMF_IDX].strptr);
              char *save = NULL;
              char *address = strtok_r(cidr, "/", &save);
              strcpy(NGAP_REGISTER_GNB_REQ(msg_p).gnb_ip_address.ipv4_address, address);
              LOG_I(GNB_APP, "Parsed gNB local IPv4 address for NG AMF: %s\n", address);
            } else {
              int sock = socket(AF_INET, SOCK_DGRAM, 0);
              if (sock < 0) {
                LOG_E(GNB_APP, "Failed to create socket for auto-detecting gNB IPv4 address\n");
              }

              struct sockaddr_in dst;
              dst.sin_family = AF_INET;
              dst.sin_port = htons(0);
              inet_pton(AF_INET, NGAP_REGISTER_GNB_REQ(msg_p).amf_ip_address[0].ipv4_address, &dst.sin_addr);
              connect(sock, (struct sockaddr *)&dst, sizeof(dst));
              if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
                LOG_E(GNB_APP, "Failed to connect socket for auto-detecting gNB IPv4 address\n");
                close(sock);
              }

              struct sockaddr_in src;
              socklen_t len = sizeof(src);
              getsockname(sock, (struct sockaddr *)&src, &len);
              char buf[INET_ADDRSTRLEN];
              inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf));
              close(sock);
              strcpy(NGAP_REGISTER_GNB_REQ(msg_p).gnb_ip_address.ipv4_address, buf);
              LOG_I(GNB_APP, "Auto-detected  gNB local IPv4 address for NG AMF: %s\n", buf);
            }

            NGAP_REGISTER_GNB_REQ(msg_p).gnb_ip_address.ipv6 = 0;
            NGAP_REGISTER_GNB_REQ(msg_p).gnb_ip_address.ipv4 = 1;

            break;
          }
        }
      }
    }
  }
  return 0;
}
