/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef __RRC_GNB_NRDC_H__
#define __RRC_GNB_NRDC_H__

#include "nr_rrc_defs.h"
#include "NR_DL-DCCH-Message.h"
#include "NR_UL-DCCH-Message.h"

void rrc_gnb_nrdc_start(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue);
void rrc_gnb_nrdc_timeout(gNB_RRC_INST *rrc, nr_rrc_nrdc_timeout_t *timeout);

#endif /* __RRC_GNB_NRDC_H__ */
