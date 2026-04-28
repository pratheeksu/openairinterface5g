
#include <sys/types.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "telnetsrv.h"

#include "openair2/RRC/NR/nr_rrc_defs.h"
#include "openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h"
#include "openair2/LAYER2/NR_MAC_gNB/nr_radio_config.h"
#include "openair2/LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.c"
#include "common/utils/nr/nr_common.h"



#define ERROR_MSG_RET(mSG, aRGS...) do { prnt("FAILURE: " mSG, ##aRGS); return 1; } while (0)

#define ISINITBWP "bwp3gpp:isInitialBwp"
//#define CYCLPREF  "bwp3gpp:cyclicPrefix"
#define NUMRBS    "bwp3gpp:numberOfRBs"
#define STARTRB   "bwp3gpp:startRB"
#define BWPSCS    "bwp3gpp:subCarrierSpacing"

#define SSBFREQ "nrcelldu3gpp:ssbFrequency"
#define ARFCNDL "nrcelldu3gpp:arfcnDL"
#define BWDL    "nrcelldu3gpp:bSChannelBwDL"
#define ARFCNUL "nrcelldu3gpp:arfcnUL"
#define BWUL    "nrcelldu3gpp:bSChannelBwUL"
#define PCI     "nrcelldu3gpp:nRPCI"
#define TAC     "nrcelldu3gpp:nRTAC"
#define MCC     "nrcelldu3gpp:mcc"
#define MNC     "nrcelldu3gpp:mnc"
#define SD      "nrcelldu3gpp:sd"
#define SST     "nrcelldu3gpp:sst"
#define SSBSCS  "nrcelldu3gpp:ssbSubCarrierSpacing"
#define SSBPRD  "nrcelldu3gpp:ssbPeriodicity"
#define SSBOFF  "nrcelldu3gpp:ssbOffset"
#define SSBDUR  "nrcelldu3gpp:ssbDuration"
#define PMAX "nrfreqrel3gpp:pMax"
#define CELLLOCALID "nrcellcu3gpp:cellLocalId"
#define CU_MCC "nrcellcu3gpp:mcc"
#define CU_MNC "nrcellcu3gpp:mnc"
#define CU_SST "nrcellcu3gpp:sst"
#define CU_SD  "nrcellcu3gpp:sd"

#define PRINTLIST_i(len, fmt, ...) \
  { \
    for (int i = 0; i < len; ++i) { \
      if (i != 0) prnt(", "); \
      prnt(fmt, __VA_ARGS__); \
    } \
  } \

typedef struct b {
  long int dl;
  long int ul;
  long int unres_ul;
  long int unres_dl;
} b_t;

typedef struct ue_stat {
  rnti_t rnti;
  b_t thr;
} ue_stat_t;
