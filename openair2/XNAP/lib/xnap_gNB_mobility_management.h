/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef XNAP_GNB_MOBILITY_MANAGEMENT_H_
#define XNAP_GNB_MOBILITY_MANAGEMENT_H_

#include "xnap_messages_types.h"
typedef struct XNAP_XnAP_PDU XNAP_XnAP_PDU_t;

XNAP_XnAP_PDU_t *encode_xnap_handover_request(xnap_handover_req_t *xnap_handover_req);
XNAP_XnAP_PDU_t *encode_xnap_handover_request_acknowledge(xnap_handover_req_ack_t *xnap_handover_req_ack);

bool decode_xnap_handover_request(xnap_handover_req_t *req, const XNAP_XnAP_PDU_t *pdu);
bool decode_xnap_handover_request_acknowledge(xnap_handover_req_ack_t *out, const XNAP_XnAP_PDU_t *pdu);

bool eq_xnap_handover_request(const xnap_handover_req_t *a, const xnap_handover_req_t *b);
bool eq_xnap_handover_request_acknowledge(const xnap_handover_req_ack_t *a, const xnap_handover_req_ack_t *b);

void free_xnap_handover_request(const xnap_handover_req_t *msg);
void free_xnap_handover_request_acknowledge(const xnap_handover_req_ack_t *msg);

#endif // XNAP_GNB_MOBILITY_MANAGEMENT_H_
