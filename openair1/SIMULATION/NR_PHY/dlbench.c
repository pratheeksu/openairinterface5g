/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nr_dlbench: Standalone DL scheduler + PHY TX benchmark
 *
 * Measures wall-clock time of the full DL pipeline
 * (scheduler → LDPC encode → modulate → OFDM) as a function of the number
 * of UEs, using the real MAC scheduler and real PHY TX chain.
 *
 * Usage:
 *   nr_dlbench -n <num_slots> -u <num_ues> -R <N_RB_DL> -m <numerology>
 *              -e <mcs> -P (print perf) -L <log_level> -X <thread_pool>
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/assertions.h"
#include "common/utils/nr/nr_common.h"
#include "executables/nr-uesoftmodem.h"
#include "executables/softmodem-common.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "LAYER2/NR_MAC_gNB/mac_rrc_dl_handler.h"
#include "LAYER2/NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/nr_radio_config.h"
#include "NR_CellGroupConfig.h"
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_COMMON/nr_mac_common.h"
#include "NR_PHY_INTERFACE/NR_IF_Module.h"
#include "NR_LogicalChannelConfig.h"
#include "NR_ReconfigurationWithSync.h"
#include "NR_RLC-BearerConfig.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/INIT/nr_phy_init.h"
#include "PHY/MODULATION/modulation_common.h"
#include "PHY/NR_TRANSPORT/nr_dlsch.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/TOOLS/tools_defs.h"
#include "PHY/defs_gNB.h"
#include "PHY/defs_nr_common.h"
#include "PHY/impl_defs_nr.h"
#include "SCHED_NR/sched_nr.h"
#include "common/config/config_load_configmodule.h"
#include "common/ngran_types.h"
#include "common/ran_context.h"
#include "common/utils/T/T.h"
#include "e1ap_messages_types.h"
#include "nfapi_nr_interface_scf.h"
#include "openair1/SIMULATION/NR_PHY/nr_unitary_defs.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "thread-pool.h"
#include "time_meas.h"

/* ── Globals required by OAI framework ── */
PHY_VARS_gNB *gNB;
RAN_CONTEXT_t RC;
int64_t uplink_frequency_offset[MAX_NUM_CCs][4];
double cpuf;
char *uecap_file;
uint64_t downlink_frequency[MAX_NUM_CCs][4];
THREAD_STRUCT thread_struct;
nfapi_ue_release_request_body_t release_rntis;
instance_t DUuniqInstance = 0;
instance_t CUuniqInstance = 0;
unsigned int NTN_UE_Koffset = 0;
configmodule_interface_t *uniqCfg = NULL;

/* PHY_vars_UE_g referenced by probe.c (LTE UTIL lib) */
void *PHY_vars_UE_g = NULL;

/* Stubs for symbols pulled in by the linker but not needed */
void signal_rrc_msg(void) { abort(); }
void signal_rrc_state_changed_to(void) { abort(); }
void signal_ue_id(void) { abort(); }

/* E1AP stubs (pulled in by cucp_cuup_direct.c in libe1_if) */
void e1_bearer_context_setup(const e1ap_bearer_setup_req_t *req) { abort(); }
void e1_bearer_context_modif(const e1ap_bearer_mod_req_t *req) { abort(); }
struct e1ap_bearer_release_cmd_s;
void e1_bearer_release_cmd(const struct e1ap_bearer_release_cmd_s *cmd) { abort(); }

/* UE params stub (referenced transitively by UE-side libs) */
nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void) { return &nrUE_params; }

/* ── Timing helpers ── */
static inline double timespec_diff_us(struct timespec *start, struct timespec *end)
{
  return (end->tv_sec - start->tv_sec) * 1e6 + (end->tv_nsec - start->tv_nsec) / 1e3;
}

static int bench_target_mcs = 28;
static int bench_target_layers = 1;

/* Print one row of the latency breakdown table.
 * Shows: total time across all slots, per-trial average, max, and trial count. */
static void print_meas_row(const char *name, time_stats_t *ts, int n_slots)
{
  double cpu_ghz = get_cpu_freq_GHz();
  if (ts->trials == 0) {
    printf("  %-34s       -           -           -       0\n", name);
    return;
  }
  double total_us = (double)ts->diff / cpu_ghz / 1000.0;
  double per_slot_us = total_us / n_slots;
  double avg_us = total_us / ts->trials;
  double max_us = (double)ts->max / cpu_ghz / 1000.0;
  printf("  %-34s %7.1f   %7.1f   %7.1f   %5d\n", name, per_slot_us, avg_us, max_us, ts->trials);
}

/* No-op UL preprocessor: we only benchmark DL */
static void bench_ul_preprocessor_noop(gNB_MAC_INST *mac, post_process_pusch_t *pp_pusch)
{
  (void)mac;
  (void)pp_pusch;
}

/* Push SDU data into each UE's SRB 1 RLC entity so the real scheduler
 * sees loaded buffers via nr_mac_rlc_status_ind(). */
static void refill_rlc_buffers(gNB_MAC_INST *mac, int bytes_per_ue)
{
  UE_iterator(mac->UE_info.connected_ue_list, UE) {
    uint8_t *sdu = malloc(bytes_per_ue);
    AssertFatal(sdu, "malloc failed\n");
    memset(sdu, 0xAB, bytes_per_ue);
    protocol_ctxt_t ctxt = {.rntiMaybeUEid = UE->rnti, .enb_flag = 1, .module_id = 0};
    nr_rlc_data_req(&ctxt, 1 /* srb_flag=1 */, 1 /* rb_id=1 (SRB1) */, 0, bytes_per_ue, sdu);
    /* nr_rlc_data_req frees sdu */
  }
}

/* ── Main ── */
int main(int argc, char **argv)
{
  stop = false;
  struct sigaction oldaction;
  sigaction(SIGINT, &sigint_action, &oldaction);

  setbuf(stdout, NULL);
  cpuf = get_cpu_freq_GHz();

  /* defaults */
  int N_RB_DL = 106;
  int mu = 1;
  int n_slots = 100;
  int num_ues = 1;
  int loglvl = OAILOG_WARNING;
  char gNBthreads[128] = "n";
  char *csv_filename = NULL;
  int print_perf = 0;
  double target_bler = 0.0;
  unsigned int seed = 0;
  bool seed_set = false;

  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == 0) {
    printf("Error: configuration module init failed\n");
    return 1;
  }

  int c;
  while ((c = getopt(argc, argv, "--:O:u:n:R:m:e:L:X:Z:B:S:Ph")) != -1) {
    if (c == 1 || c == '-' || c == 'O')
      continue;
    switch (c) {
      case 'u':
        num_ues = atoi(optarg);
        break;
      case 'n':
        n_slots = atoi(optarg);
        break;
      case 'R':
        N_RB_DL = atoi(optarg);
        break;
      case 'm':
        mu = atoi(optarg);
        break;
      case 'e':
        bench_target_mcs = atoi(optarg);
        break;
      case 'L':
        loglvl = atoi(optarg);
        break;
      case 'X':
        strncpy(gNBthreads, optarg, sizeof(gNBthreads) - 1);
        break;
      case 'Z':
        csv_filename = strdup(optarg);
        break;
      case 'B':
        target_bler = atof(optarg);
        break;
      case 'S':
        seed = (unsigned int)atol(optarg);
        seed_set = true;
        break;
      case 'P':
        print_perf = 1;
        cpu_meas_enabled = 1;
        break;
      case 'h':
      default:
        printf("nr_dlbench - DL scheduler + PHY TX benchmark\n\n");
        printf("  -u <num_ues>     Number of UEs (default 1, max %d)\n", MAX_MOBILES_PER_GNB);
        printf("  -n <num_slots>   Number of DL slots to benchmark (default 100)\n");
        printf("  -R <N_RB_DL>     Number of DL resource blocks (default 106)\n");
        printf("  -m <numerology>  Numerology index (default 1 = 30kHz)\n");
        printf("  -e <mcs>         Max MCS index cap (default 28, scheduler adapts)\n");
        printf("  -L <log_level>   Log level 0-5 (default 1=warning)\n");
        printf("  -X <threads>     gNB thread pool config (default 'n' = no threads)\n");
        printf("  -Z <file.csv>    Output CSV file for per-slot timing\n");
        printf("  -B <bler>        Simulated DL BLER (default 0.0 = all ACK)\n");
        printf("  -S <seed>        RNG seed for reproducibility (default: random)\n");
        printf("  -P               Print PHY performance counters\n");
        printf("  -h               This help\n");
        return 0;
    }
  }

  if (num_ues > MAX_MOBILES_PER_GNB) {
    printf("Error: num_ues %d exceeds MAX_MOBILES_PER_GNB (%d). "
           "Raise the limit in common/openairinterface5g_limits.h and rebuild.\n",
           num_ues, MAX_MOBILES_PER_GNB);
    return 1;
  }

  logInit();
  set_glog(loglvl);
  InitSinLUT();
  randominit();

  if (!seed_set)
    seed = (unsigned int)time(NULL);
  srand(seed);

  printf("=== nr_dlbench: %d UEs, %d RBs, mu=%d, MCS=%d, %d slots, BLER=%.2f, seed=%u ===\n",
         num_ues, N_RB_DL, mu, bench_target_mcs, n_slots, target_bler, seed);

  /* ── 1. Allocate PHY ── */
  get_softmodem_params()->phy_test = 1;
  get_softmodem_params()->do_ra = 0;

  RC.gNB = calloc(1, sizeof(PHY_VARS_gNB *));
  RC.gNB[0] = calloc(1, sizeof(PHY_VARS_gNB));
  gNB = RC.gNB[0];
  gNB->ofdm_offset_divisor = UINT_MAX;
  gNB->phase_comp = true;

  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  frame_parms->nb_antennas_tx = 1;
  frame_parms->nb_antennas_rx = 1;
  frame_parms->N_RB_DL = N_RB_DL;
  frame_parms->N_RB_UL = N_RB_DL;

  AssertFatal((gNB->if_inst = NR_IF_Module_init(0)) != NULL, "Cannot register interface");
  gNB->if_inst->NR_PHY_config_req = nr_phy_config_request;

  /* ── 2. Create ServingCellConfigCommon ── */
  NR_ServingCellConfigCommon_t *scc = calloc(1, sizeof(*scc));
  prepare_scc(scc);
  uint64_t ssb_bitmap = 1;
  fill_scc_sim(scc, &ssb_bitmap, N_RB_DL, N_RB_DL, mu, mu);
  fix_scc(scc, ssb_bitmap);

  frame_structure_t frame_structure = {0};
  frame_type_t frame_type = TDD;
  config_frame_structure(mu,
                         scc->tdd_UL_DL_ConfigurationCommon,
                         get_tdd_period_idx(scc->tdd_UL_DL_ConfigurationCommon),
                         frame_type,
                         &frame_structure);

  /* ── 3. Init MAC ── */
  nr_pdsch_AntennaPorts_t pdsch_AntennaPorts = {.N1 = 1, .N2 = 1, .XP = 1};
  const nr_mac_config_t conf = {
      .pdsch_AntennaPorts = pdsch_AntennaPorts,
      .pusch_AntennaPorts = 1,
      .minRXTXTIME = 6,
      .do_CSIRS = 0,
      .do_SRS = 0,
      .num_dlharq = 16,
      .num_ulharq = 16,
      .maxMIMO_layers = bench_target_layers,
      .force_256qam_off = false,
      .timer_config.sr_ProhibitTimer = 0,
      .timer_config.sr_TransMax = 64,
      .timer_config.sr_ProhibitTimer_v1700 = 0,
      .timer_config.t300 = 400,
      .timer_config.t301 = 400,
      .timer_config.t310 = 2000,
      .timer_config.n310 = 10,
      .timer_config.t311 = 3000,
      .timer_config.n311 = 1,
      .timer_config.t319 = 400,
      .num_agg_level_candidates = {0, 0, 1, 1, 0},
  };
  const nr_rlc_configuration_t rlc_config = {
      .srb = {.t_poll_retransmit = 45, .t_reassembly = 35, .t_status_prohibit = 0,
              .poll_pdu = -1, .poll_byte = -1, .max_retx_threshold = 8, .sn_field_length = 12},
      .drb_am = {.t_poll_retransmit = 45, .t_reassembly = 15, .t_status_prohibit = 15,
                 .poll_pdu = 64, .poll_byte = 1024 * 500, .max_retx_threshold = 32, .sn_field_length = 18},
      .drb_um = {.t_reassembly = 15, .sn_field_length = 12},
  };

  RC.nb_nr_macrlc_inst = 1;
  mac_top_init_gNB(ngran_gNB, scc, &conf, &rlc_config);
  gNB_MAC_INST *gNB_mac = RC.nrmac[0];
  nr_mac_config_scc(gNB_mac, scc, &conf);

  gNB_mac->dl_bler.harq_round_max = 4;
  gNB_mac->ul_bler.harq_round_max = 4;
  gNB->ap_N1 = pdsch_AntennaPorts.N1;
  gNB->ap_N2 = pdsch_AntennaPorts.N2;
  gNB->ap_XP = pdsch_AntennaPorts.XP;

  /* ── 4. Use the real DL scheduler instead of the phytest one ── */
  gNB_mac->pre_processor_dl = nr_init_dlsch_preprocessor(0);
  gNB_mac->pre_processor_ul = bench_ul_preprocessor_noop;

  /* ── 5. Init PHY ── */
  phy_init_nr_gNB(gNB);
  N_RB_DL = gNB->frame_parms.N_RB_DL;

  /* ── 6. Create N test UEs ── */
  printf("Creating %d test UEs...\n", num_ues);
  for (int u = 0; u < num_ues; u++) {
    rnti_t rnti = 0x1234 + u;
    NR_CellGroupConfig_t *cg = get_default_secondaryCellGroup(scc, NULL, 0, 1, &conf, u, 0);
    cg->spCellConfig->reconfigurationWithSync = get_reconfiguration_with_sync(rnti, u, scc, 0);

    bool ok = nr_mac_add_test_ue(gNB_mac, rnti, cg);
    AssertFatal(ok, "Failed to add test UE %d (rnti=%04x)\n", u, rnti);

    NR_UE_info_t *UE_info = gNB_mac->UE_info.connected_ue_list[u];
    AssertFatal(UE_info != NULL, "UE %d not found in connected list\n", u);
    configure_UE_BWP(gNB_mac, scc, UE_info, false, NR_SearchSpace__searchSpaceType_PR_ue_Specific, -1, -1);

    /* Create RLC SRB 1 entity for this UE.
     * nr_mac_add_test_ue only adds the LCID to MAC scheduling, not the RLC entity.
     * We create a temporary RLC bearer config, add the entity, then free it. */
    NR_RLC_BearerConfig_t *srb1_rlc_cfg = get_SRB_RLC_BearerConfig(
        1, 1, NR_LogicalChannelConfig__ul_SpecificParameters__bucketSizeDuration_ms1000, &rlc_config);
    nr_rlc_add_srb(rnti, 1, srb1_rlc_cfg);
    ASN_STRUCT_FREE(asn_DEF_NR_RLC_BearerConfig, srb1_rlc_cfg);

    /* Set UE channel quality so the scheduler picks a high MCS */
    NR_UE_sched_ctrl_t *sc = &UE_info->UE_sched_ctrl;
    sc->dl_max_mcs = bench_target_mcs;

    printf("  UE %3d: rnti=%04x, BWP size=%d\n", u, rnti, UE_info->current_DL_BWP.BWPSize);
  }

  /* ── 7. Init thread pool ── */
  initNamedTpool(gNBthreads, &gNB->threadPool, true, "gNB-tpool");
  initNotifiedFIFO(&gNB->L1_tx_out);

  /* ── 8. Allocate sched response ── */
  NR_Sched_Rsp_t *sched_rsp = calloc(1, sizeof(*sched_rsp));
  AssertFatal(sched_rsp, "alloc failed\n");

  /* ── 9. Open CSV output ── */
  FILE *csv = NULL;
  if (csv_filename) {
    csv = fopen(csv_filename, "w");
    if (!csv) {
      printf("Warning: cannot open %s for writing\n", csv_filename);
    } else {
      fprintf(csv, "slot,frame,num_ues,n_pdsch,sched_us,phy_tx_us,total_us\n");
    }
  }

  /* ── 10. Find DL slots in the TDD pattern ── */
  int dl_slots[40];
  int n_dl_slots = 0;
  for (int s = 0; s < (int)frame_structure.numb_slots_period && n_dl_slots < 40; s++) {
    if (is_dl_slot(s, &frame_structure))
      dl_slots[n_dl_slots++] = s;
  }
  printf("TDD period has %d DL slots out of %d total\n", n_dl_slots, frame_structure.numb_slots_period);
  if (n_dl_slots == 0) {
    printf("Error: no DL slots found in TDD pattern!\n");
    return 1;
  }

  /* ── 11. Benchmark loop ── */
  printf("\nRunning %d DL slot iterations...\n\n", n_slots);

  double *t_sched = calloc(n_slots, sizeof(double));
  double *t_phy = calloc(n_slots, sizeof(double));
  double *t_total = calloc(n_slots, sizeof(double));
  int *n_pdsch_arr = calloc(n_slots, sizeof(int));

  struct timespec ts0, ts1, ts2;

  int frame = 0;
  int dl_slot_idx = 0;

  for (int iter = 0; iter < n_slots && !stop; iter++) {
    int slot = dl_slots[dl_slot_idx];

    /* ── Pre-slot: inject synthetic UE state ── */

    /* 1) Simulate HARQ feedback: ACK or NACK based on target_bler */
    UE_iterator(gNB_mac->UE_info.connected_ue_list, UE) {
      NR_UE_sched_ctrl_t *sc = &UE->UE_sched_ctrl;
      while (sc->feedback_dl_harq.head >= 0) {
        int pid = sc->feedback_dl_harq.head;
        remove_front_nr_list(&sc->feedback_dl_harq);
        bool nack = (target_bler > 0.0) && ((rand() / (double)RAND_MAX) < target_bler);
        if (nack && sc->harq_processes[pid].round < gNB_mac->dl_bler.harq_round_max - 1) {
          /* NACK: bump round, send to retransmission */
          sc->harq_processes[pid].round++;
          sc->harq_processes[pid].is_waiting = false;
          add_tail_nr_list(&sc->retrans_dl_harq, pid);
        } else {
          /* ACK (or max rounds reached): recycle */
          sc->harq_processes[pid].round = 0;
          sc->harq_processes[pid].ndi ^= 1;
          sc->harq_processes[pid].is_waiting = false;
          add_tail_nr_list(&sc->available_dl_harq, pid);
        }
      }
      /* Also drain any retrans that got stuck from previous iterations */
      /* (retrans are re-scheduled by the scheduler, not recycled here) */
    }

    /* 2) Push data into RLC so the scheduler sees loaded buffers */
    refill_rlc_buffers(gNB_mac, 10000);

    /* Reset sched response (gNB_dlsch_ulsch_scheduler clears nFAPI internally) */
    reset_sched_response(sched_rsp, frame, slot, 0, 0);

    /* ── PHASE 1: Full scheduler (RA + UL + DL + PDCCH + everything) ── */
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    gNB_dlsch_ulsch_scheduler(0, frame, slot, sched_rsp);

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    int n_pdsch = 0;
    for (int p = 0; p < sched_rsp->DL_req.dl_tti_request_body.nPDUs; p++) {
      if (sched_rsp->DL_req.dl_tti_request_body.dl_tti_pdu_list[p].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
        n_pdsch++;
    }

    /* ── PHASE 2: Full PHY TX (SSB + PDCCH + PDSCH + phase comp) ── */
    phy_procedures_gNB_TX(gNB, &sched_rsp->DL_req, &sched_rsp->TX_req, &sched_rsp->UL_dci_req, frame, slot);

    clock_gettime(CLOCK_MONOTONIC, &ts2);

    /* Record timings */
    t_sched[iter] = timespec_diff_us(&ts0, &ts1);
    t_phy[iter] = timespec_diff_us(&ts1, &ts2);
    t_total[iter] = timespec_diff_us(&ts0, &ts2);
    n_pdsch_arr[iter] = n_pdsch;

    if (csv) {
      fprintf(csv, "%d,%d,%d,%d,%.1f,%.1f,%.1f\n",
              slot, frame, num_ues, n_pdsch, t_sched[iter], t_phy[iter], t_total[iter]);
    }

    if (iter < 5 || (iter % 20 == 0)) {
      printf("  slot %3d [%d.%d]: %d PDSCH | sched %7.1f us | phy_tx %7.1f us | total %7.1f us\n",
             iter, frame, slot, n_pdsch, t_sched[iter], t_phy[iter], t_total[iter]);
    }

    /* advance to next DL slot */
    dl_slot_idx++;
    if (dl_slot_idx >= n_dl_slots) {
      dl_slot_idx = 0;
      frame = (frame + 1) % 1024;
    }
  }

  /* ── 12. Statistics ── */

  /* skip first 5 slots as warmup */
  int warmup = n_slots > 10 ? 5 : 0;
  int count = n_slots - warmup;
  int total_pdsch = 0;
  for (int i = warmup; i < n_slots; i++)
    total_pdsch += n_pdsch_arr[i];

  /* sort copies for percentile computation */
  double *s_sched = malloc(count * sizeof(double));
  double *s_phy = malloc(count * sizeof(double));
  double *s_total = malloc(count * sizeof(double));
  memcpy(s_sched, t_sched + warmup, count * sizeof(double));
  memcpy(s_phy, t_phy + warmup, count * sizeof(double));
  memcpy(s_total, t_total + warmup, count * sizeof(double));

  int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
  }
  qsort(s_sched, count, sizeof(double), cmp_double);
  qsort(s_phy, count, sizeof(double), cmp_double);
  qsort(s_total, count, sizeof(double), cmp_double);

  /* find which iteration had the max total */
  int max_total_iter = warmup;
  for (int i = warmup; i < n_slots; i++)
    if (t_total[i] > t_total[max_total_iter])
      max_total_iter = i;

  /* slot duration: 1000us for mu=0, 500us for mu=1, 250us for mu=2 */
  double slot_dur_us = 1000.0 / (1 << mu);

  double mean_sched = 0, mean_phy = 0, mean_total = 0;
  for (int i = 0; i < count; i++) {
    mean_sched += s_sched[i];
    mean_phy += s_phy[i];
    mean_total += s_total[i];
  }
  mean_sched /= count;
  mean_phy /= count;
  mean_total /= count;

#define P(arr, pct) (arr[(int)((pct) / 100.0 * (count - 1))])

  printf("\n=== Results: %d UEs, %d RBs, MCS %d, %d slots (warmup %d) ===\n",
         num_ues, N_RB_DL, bench_target_mcs, n_slots, warmup);
  printf("  Avg PDSCH/slot: %.1f / %d UEs\n", (double)total_pdsch / count, num_ues);
  printf("  Slot budget: %.0f us (mu=%d)\n\n", slot_dur_us, mu);

  printf("  %-12s %8s %8s %8s %8s %8s\n", "Phase", "mean", "p50", "p90", "p99", "max");
  printf("  %-12s %8s %8s %8s %8s %8s\n", "", "(us)", "(us)", "(us)", "(us)", "(us)");
  printf("  ------------------------------------------------------------------\n");
  printf("  %-12s %8.1f %8.1f %8.1f %8.1f %8.1f\n", "Scheduler", mean_sched, P(s_sched, 50), P(s_sched, 90), P(s_sched, 99), s_sched[count - 1]);
  printf("  %-12s %8.1f %8.1f %8.1f %8.1f %8.1f\n", "PHY TX", mean_phy, P(s_phy, 50), P(s_phy, 90), P(s_phy, 99), s_phy[count - 1]);
  printf("  %-12s %8.1f %8.1f %8.1f %8.1f %8.1f\n", "Total", mean_total, P(s_total, 50), P(s_total, 90), P(s_total, 99), s_total[count - 1]);
  printf("  ------------------------------------------------------------------\n");
  printf("  Max total %.1f us at slot %d (iter %d)\n", t_total[max_total_iter], dl_slots[max_total_iter % n_dl_slots], max_total_iter);

#undef P

  if (mean_total > slot_dur_us)
    printf("\n  WARNING: mean total (%.0f us) exceeds slot budget (%.0f us)!\n",
           mean_total, slot_dur_us);
  if (s_total[count - 1] > slot_dur_us)
    printf("  WARNING: max total (%.0f us) exceeds slot budget (%.0f us)!\n",
           s_total[count - 1], slot_dur_us);

  free(s_sched);
  free(s_phy);
  free(s_total);

  /* ── 13. Print detailed latency breakdown ── */
  if (print_perf) {
    printf("\n  %-34s %7s   %7s   %7s   %5s\n", "Breakdown", "/slot", "/call", "max", "calls");
    printf("  %-34s %7s   %7s   %7s   %5s\n", "", "(us)", "(us)", "(us)", "");
    printf("  ---------------------------------------------------------------------------\n");

    printf("  Scheduler:\n");
    print_meas_row("  Total", &gNB_mac->gNB_scheduler, count);
    print_meas_row("    RA scheduling", &gNB_mac->schedule_ra, count);
    print_meas_row("    UL scheduling", &gNB_mac->schedule_ulsch, count);
    print_meas_row("    DL scheduling (PDCCH+PDSCH)", &gNB_mac->schedule_dlsch, count);
    print_meas_row("      RLC data req", &gNB_mac->rlc_data_req, count);

    printf("  PHY TX:\n");
    print_meas_row("  Total", &gNB->phy_proc_tx, count);
    print_meas_row("    DCI generation", &gNB->dci_generation_stats, count);
    print_meas_row("    DLSCH encoding", &gNB->dlsch_encoding_stats, count);
    print_meas_row("      segmentation", &gNB->dlsch_segmentation_stats, count);
    print_meas_row("      rate matching", &gNB->dlsch_rate_matching_stats, count);
    print_meas_row("      scrambling", &gNB->dlsch_scrambling_stats, count);
    print_meas_row("    DLSCH modulation", &gNB->dlsch_modulation_stats, count);
    print_meas_row("    layer mapping", &gNB->dlsch_layer_mapping_stats, count);
    print_meas_row("    precoding", &gNB->dlsch_precoding_stats, count);
    print_meas_row("    resource mapping", &gNB->dlsch_resource_mapping_stats, count);
    print_meas_row("    phase compensation", &gNB->phase_comp_stats, count);
    printf("  ---------------------------------------------------------------------------\n");
  }

  /* ── Cleanup ── */
  if (csv) fclose(csv);
  free(t_sched);
  free(t_phy);
  free(t_total);
  free(n_pdsch_arr);
  free(sched_rsp);
  free(csv_filename);

  printf("\nDone.\n");
  return 0;
}
