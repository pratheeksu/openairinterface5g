/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "rrc_gNB_measurements.h"
#include "nr_rrc_defs.h"

int allocate_measurement_object_id(void *_ue)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = 1;
  int ret;
  for (ret = 0; ret < 64; ret++, mask <<= 1)
    if (!(ue->measurement_object_ids & mask))
      break;
  DevAssert(ret < 64);
  ue->measurement_object_ids |= mask;
  return ret + 1;
}

void free_measurement_object_id(void *_ue, int mo_id)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = (uint64_t)1 << (uint64_t)(mo_id - 1);
  DevAssert(ue->measurement_object_ids & mask);
  ue->measurement_object_ids &= ~mask;
}

int allocate_measurement_id(void *_ue)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = 1;
  int ret;
  for (ret = 0; ret < 64; ret++, mask <<= 1)
    if (!(ue->measurement_ids & mask))
      break;
  DevAssert(ret < 64);
  ue->measurement_ids |= mask;
  return ret + 1;
}

void free_measurement_id(void *_ue, int mo_id)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = (uint64_t)1 << (uint64_t)(mo_id - 1);
  DevAssert(ue->measurement_ids & mask);
  ue->measurement_ids &= ~mask;
}

int allocate_report_config_id(void *_ue)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = 1;
  int ret;
  for (ret = 0; ret < 64; ret++, mask <<= 1)
    if (!(ue->report_config_ids & mask))
      break;
  DevAssert(ret < 64);
  ue->report_config_ids |= mask;
  return ret + 1;
}

void free_report_config_id(void *_ue, int mo_id)
{
  gNB_RRC_UE_t *ue = _ue;
  uint64_t mask = (uint64_t)1 << (uint64_t)(mo_id - 1);
  DevAssert(ue->report_config_ids & mask);
  ue->report_config_ids &= ~mask;
}
