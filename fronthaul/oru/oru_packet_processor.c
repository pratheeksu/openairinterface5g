#include "common/platform_types.h"
#include "xran_pkt_api.h"
#include "oru_packet_processor.h"
#include <rte_byteorder.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "assertions.h"
#include "log.h"
#include <rte_ring.h>
#include "common/utils/nr/nr_common.h"
#include <sys/types.h>

#define MAX_CONCURRENT_SYMBOL_WINDOWS 1024
#define MAX_ANTENNAS 4
#define NR_NUMBER_OF_SUBFRAMES_PER_FRAME 10
#define MAX_TDD_PATTERN_LENGTH_MS 10
#define MAX_SLOTS_PER_MS 4
#define SYMBOL_BITMASK_SIZE ((NR_SYMBOLS_PER_SLOT * MAX_TDD_PATTERN_LENGTH_MS * MAX_SLOTS_PER_MS + 7) / 8)
#define MAX_RX_FRAGMENTS 4
#define MAX_MBUFS_PER_SYMBOL 64

typedef enum {
  SYM_DL_IDLE,
  SYM_DL_ACTIVE,
  SYM_DL_READY,
  SYM_DL_EMPTY,
} dl_symbol_job_state_t;

typedef enum {
  SYM_UL_IDLE,
  SYM_UL_ACTIVE,
  SYM_UL_READY,
  SYM_UL_EMPTY,
} ul_symbol_job_state_t;

typedef struct {
  struct {
    bool cplane_received;
    int section_id;
    struct {
      int start_prbc;
      int num_prbc;
      void *iq_data;
      void *mbuf;
    } rx_fragments[MAX_RX_FRAGMENTS];
    int num_rx_fragments;
  } per_antenna[MAX_ANTENNAS];
  dl_symbol_job_state_t state;
  int expected_iq;
  int received_iq;
  uint64_t absolute_symbol;
} dl_symbol_job_t;

typedef struct {
  struct {
    bool cplane_received;
    int section_id;
    int num_prb;
    int start_prb;
  } per_antenna[MAX_ANTENNAS];
  uint64_t absolute_symbol;
  ul_symbol_job_state_t state;
} ul_symbol_job_t;

typedef struct {
  _Atomic(uint64_t) dl_tdd_mismatch;
  _Atomic(uint64_t) ul_tdd_mismatch;
  _Atomic(uint64_t) ul_cplane_missing;
  _Atomic(uint64_t) out_of_mbufs;
  _Atomic(uint64_t) total_uplane_sent;
} thread_safe_stats_t;

typedef struct {
  dl_symbol_job_t dl_symbol_jobs[MAX_CONCURRENT_SYMBOL_WINDOWS];
  ul_symbol_job_t ul_symbol_jobs[MAX_CONCURRENT_SYMBOL_WINDOWS];
  uint64_t current_absolute_symbol;
  uint64_t last_pushed_symbol;
  struct rte_ring *dl_ready_jobs;
  uint32_t T2a_min_cp_dl_sym_diff;
  uint32_t T2a_max_cp_dl_sym_diff;
  uint32_t T2a_min_up_dl_sym_diff;
  uint32_t T2a_max_up_dl_sym_diff;
  struct xran_eaxcid_config eaxcid_config;
  int prach_eaxc_offset;
  int numerology;
  int num_prb;
  oru_packet_processor_stats_t stats;
  thread_safe_stats_t thread_safe_stats;
  uint8_t dl_symbol_bitmask[SYMBOL_BITMASK_SIZE];
  uint8_t ul_symbol_bitmask[SYMBOL_BITMASK_SIZE];
  uint16_t symbol_bitmask_length;
  alloc_func_t alloc_func;
  send_func_t send_func;
  void *io_controller;
  _Atomic(uint8_t) pusch_seq_id[MAX_ANTENNAS];
  size_t mtu;
} oru_packet_processor_context_t;

static inline void set_bit(uint8_t *bits, uint64_t bit)
{
  bits[bit / 8] |= 1 << (bit % 8);
}

static inline int test_bit(uint8_t *bits, uint64_t bit)
{
  return bits[bit / 8] & (1 << (bit % 8));
}

void *init_packet_processor(int numerology,
                            int num_prb,
                            uint32_t T2a_cp_min_uS,
                            uint32_t T2a_cp_max_uS,
                            uint32_t T2a_up_min_uS,
                            uint32_t T2a_up_max_uS,
                            int num_dl_slots,
                            int num_ul_slots,
                            int num_dl_symbols,
                            int num_ul_symbols,
                            int tdd_pattern_length_slots,
                            alloc_func_t alloc_func,
                            send_func_t send_func,
                            void *io_controller,
                            size_t mtu)
{
  oru_packet_processor_context_t *context = calloc(1, sizeof(*context));
  context->alloc_func = alloc_func;
  context->send_func = send_func;
  context->io_controller = io_controller;
  context->current_absolute_symbol = 0;
  context->num_prb = num_prb;
  context->numerology = numerology;
  context->mtu = mtu;
  uint32_t slots_per_subframe = 1 << numerology;
  uint32_t symbol_duration_uS = 1000 / slots_per_subframe / 14;
  context->T2a_min_cp_dl_sym_diff = T2a_cp_min_uS / symbol_duration_uS;
  context->T2a_max_cp_dl_sym_diff = T2a_cp_max_uS / symbol_duration_uS;
  context->T2a_min_up_dl_sym_diff = T2a_up_min_uS / symbol_duration_uS;
  context->T2a_max_up_dl_sym_diff = T2a_up_max_uS / symbol_duration_uS;
  context->dl_ready_jobs = rte_ring_create("dl_ready_jobs", MAX_CONCURRENT_SYMBOL_WINDOWS, rte_socket_id(), 0);
  AssertFatal(context->dl_ready_jobs != NULL, "Failed to create ring dl_ready_jobs\n");

  context->eaxcid_config = (struct xran_eaxcid_config){.mask_cuPortId = 0xF000,
                                                       .mask_bandSectorId = 0x0F00,
                                                       .mask_ccId = 0x00F0,
                                                       .mask_ruPortId = 0x000F,
                                                       .bit_cuPortId = 12,
                                                       .bit_bandSectorId = 8,
                                                       .bit_ccId = 4,
                                                       .bit_ruPortId = 0};
  for (int i = 0; i < MAX_CONCURRENT_SYMBOL_WINDOWS; i++) {
    context->dl_symbol_jobs[i].state = SYM_DL_IDLE;
    for (int j = 0; j < MAX_ANTENNAS; j++) {
      context->dl_symbol_jobs[i].per_antenna[j].cplane_received = false;
      context->dl_symbol_jobs[i].per_antenna[j].num_rx_fragments = 0;
      for (int k = 0; k < MAX_RX_FRAGMENTS; k++) {
        context->dl_symbol_jobs[i].per_antenna[j].rx_fragments[k].iq_data = NULL;
        context->dl_symbol_jobs[i].per_antenna[j].rx_fragments[k].mbuf = NULL;
      }
    }
  }
  context->symbol_bitmask_length = tdd_pattern_length_slots * NR_SYMBOLS_PER_SLOT;
  for (int i = 0; i < num_dl_slots * NR_SYMBOLS_PER_SLOT + num_dl_symbols; i++) {
    set_bit(context->dl_symbol_bitmask, i);
  }
  int last_bit = context->symbol_bitmask_length - 1;
  for (int i = 0; i < num_ul_slots * NR_SYMBOLS_PER_SLOT + num_ul_symbols; i++) {
    set_bit(context->ul_symbol_bitmask, last_bit - i);
  }
  return context;
}

void cleanup_packet_processor(void *context)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx) {
    print_packet_processor_stats(ctx);
    if (ctx->dl_ready_jobs) {
      rte_ring_free(ctx->dl_ready_jobs);
    }
    free(ctx);
  }
}

// happens when all packets for one symbol are collected
void try_push_symbol_job(oru_packet_processor_context_t *ctx, uint64_t absolute_symbol)
{
  while (ctx->last_pushed_symbol <= absolute_symbol) {
    uint32_t job_index = ctx->last_pushed_symbol % MAX_CONCURRENT_SYMBOL_WINDOWS;
    dl_symbol_job_t *job = &ctx->dl_symbol_jobs[job_index];
    if (job->state == SYM_DL_ACTIVE) {
      if (job->expected_iq == job->received_iq) {
        job->state = SYM_DL_READY;
        int ret = rte_ring_enqueue(ctx->dl_ready_jobs, (void *)(uintptr_t)job_index);
        AssertFatal(ret == 0, "Failed to enqueue into ring dl_ready_jobs\n");
      } else {
        break;
      }
    }
    ctx->last_pushed_symbol++;
  }
}

// Happens during timer expiry
void push_symbol_job(oru_packet_processor_context_t *ctx, uint64_t absolute_symbol)
{
  while (ctx->last_pushed_symbol <= absolute_symbol) {
    uint32_t job_index = ctx->last_pushed_symbol % MAX_CONCURRENT_SYMBOL_WINDOWS;
    dl_symbol_job_t *job = &ctx->dl_symbol_jobs[job_index];
    if (job->state == SYM_DL_ACTIVE) {
      job->state = SYM_DL_READY;
      int ret = rte_ring_enqueue(ctx->dl_ready_jobs, (void *)(uintptr_t)job_index);
      AssertFatal(ret == 0, "Failed to enqueue into ring dl_ready_jobs\n");
    } else {
      bool is_dl_symbol = test_bit(ctx->dl_symbol_bitmask, ctx->last_pushed_symbol % ctx->symbol_bitmask_length);
      if (is_dl_symbol) {
        job->state = SYM_DL_EMPTY;
        job->absolute_symbol = ctx->last_pushed_symbol;
        int ret = rte_ring_enqueue(ctx->dl_ready_jobs, (void *)(uintptr_t)job_index);
        AssertFatal(ret == 0, "Failed to enqueue into ring dl_ready_jobs\n");
      }
    }
    ctx->last_pushed_symbol++;
  }
}

void handle_absolute_symbol_tick(void *context, uint64_t absolute_symbol)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx->current_absolute_symbol == 0) {
    ctx->current_absolute_symbol = absolute_symbol - 1;
    ctx->last_pushed_symbol = absolute_symbol - 1;
  }
  ctx->current_absolute_symbol = absolute_symbol;
  uint64_t window_expiry_symbol = ctx->current_absolute_symbol + ctx->T2a_min_up_dl_sym_diff;
  push_symbol_job(ctx, window_expiry_symbol);
}

void handle_uplane_packet(void *context, void *pkt)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  void *iq_data_start = NULL;
  uint8_t CC_ID = 0xFF;
  uint8_t Ant_ID = 0xFF;
  uint8_t frame_id;
  uint8_t subframe_id;
  uint8_t slot_id;
  uint8_t symb_id;
  uint8_t filter_id;
  union ecpri_seq_id seq_id;
  uint16_t num_prbu;
  uint16_t start_prbu;
  uint16_t sym_inc;
  uint16_t rb;
  uint16_t sect_id;
  int expect_comp = 0;
  uint8_t staticComp = 0;
  uint8_t compMeth = 0;
  uint8_t iqWidth = 0;
  int ret = xran_extract_iq_samples(pkt,
                                    &ctx->eaxcid_config,
                                    &iq_data_start,
                                    &CC_ID,
                                    &Ant_ID,
                                    &frame_id,
                                    &subframe_id,
                                    &slot_id,
                                    &symb_id,
                                    &filter_id,
                                    &seq_id,
                                    &num_prbu,
                                    &start_prbu,
                                    &sym_inc,
                                    &rb,
                                    &sect_id,
                                    expect_comp,
                                    staticComp,
                                    &compMeth,
                                    &iqWidth);
  if (ret == 0) {
    LOG_W(HW, "Error reading packet\n");
    rte_pktmbuf_free(pkt);
    return;
  }
  AssertFatal(Ant_ID <= MAX_ANTENNAS, "Antenna id (%d) exceeds supported value %d\n", Ant_ID, MAX_ANTENNAS);
  LOG_D(HW,
        "ORAN: U-plane packet received. CC_ID %d, Ant_ID %d, frame_id %d, subframe_id %d, slot_id %d, symb_id %d, filter_id %d, "
        "num_prbu %d, start_prbu %d, sym_inc %d, rb %d, sect_id %d, compMeth %d, iqWidth %d\n",
        CC_ID,
        Ant_ID,
        frame_id,
        subframe_id,
        slot_id,
        symb_id,
        filter_id,
        num_prbu,
        start_prbu,
        sym_inc,
        rb,
        sect_id,
        compMeth,
        iqWidth);

  AssertFatal(compMeth == 0, "Compression not supported\n");
  int mu = ctx->numerology;
  int slots_per_subframe = 1 << mu;
  int num_symbols_per_frame = NR_NUMBER_OF_SUBFRAMES_PER_FRAME * slots_per_subframe * NR_SYMBOLS_PER_SLOT;
  uint32_t current_symbol_in_frame = ctx->current_absolute_symbol % num_symbols_per_frame;
  int symbol_in_frame = NR_SYMBOLS_PER_SLOT * (slot_id + subframe_id * slots_per_subframe) + symb_id;
  int32_t diff = symbol_in_frame - current_symbol_in_frame;
  if (diff < -num_symbols_per_frame / 2) {
    diff += num_symbols_per_frame;
  } else if (diff > num_symbols_per_frame / 2) {
    diff -= num_symbols_per_frame;
  }
  if (diff > (int32_t)ctx->T2a_max_up_dl_sym_diff) {
    ctx->stats.uplane_err_early++;
    rte_pktmbuf_free(pkt);
    return;
  }
  if (diff < (int32_t)ctx->T2a_min_up_dl_sym_diff) {
    ctx->stats.uplane_err_late++;
    rte_pktmbuf_free(pkt);
    return;
  }
  uint64_t target_absolute_symbol = ctx->current_absolute_symbol + diff;
  bool is_dl_symbol = test_bit(ctx->dl_symbol_bitmask, target_absolute_symbol % ctx->symbol_bitmask_length);
  if (!is_dl_symbol) {
    ctx->stats.dl_tdd_mismatch++;
    rte_pktmbuf_free(pkt);
    return;
  }
  uint32_t job_index = target_absolute_symbol % MAX_CONCURRENT_SYMBOL_WINDOWS;
  dl_symbol_job_t *job = &ctx->dl_symbol_jobs[job_index];
  if (job->state != SYM_DL_ACTIVE) {
    if (job->state == SYM_DL_IDLE) {
      ctx->stats.uplane_missing_cplane++;
    } else {
      ctx->stats.application_too_slow++;
    }
    rte_pktmbuf_free(pkt);
    return;
  }
  if (job->absolute_symbol != target_absolute_symbol) {
    ctx->stats.uplane_err_late++;
    rte_pktmbuf_free(pkt);
    return;
  }
  if (job->per_antenna[Ant_ID].num_rx_fragments < MAX_RX_FRAGMENTS) {
    int frag_idx = job->per_antenna[Ant_ID].num_rx_fragments++;
    job->per_antenna[Ant_ID].rx_fragments[frag_idx].iq_data = iq_data_start;
    job->per_antenna[Ant_ID].rx_fragments[frag_idx].mbuf = pkt;
    job->per_antenna[Ant_ID].rx_fragments[frag_idx].start_prbc = start_prbu;
    job->per_antenna[Ant_ID].rx_fragments[frag_idx].num_prbc = num_prbu == 0 ? ctx->num_prb : num_prbu;
  } else {
    LOG_W(HW, "ORU: Dropping extra segment for Ant %d, sym %lu\n", Ant_ID, target_absolute_symbol);
    rte_pktmbuf_free(pkt);
  }
  job->received_iq += num_prbu == 0 ? ctx->num_prb : num_prbu;
  if (job->expected_iq == job->received_iq) {
    try_push_symbol_job(ctx, target_absolute_symbol);
  }
  ctx->stats.total_uplane_received++;
  return;
}

static void handle_dl_cplane_packet(oru_packet_processor_context_t *ctx,
                                    void *pkt,
                                    struct xran_cp_radioapp_section1_header *hdr,
                                    struct xran_cp_radioapp_section1 *section,
                                    int ant_id)
{
  int numerology = ctx->numerology;
  int slot_in_frame = hdr->cmnhdr.field.slotId + hdr->cmnhdr.field.subframeId * (1 << numerology);
  uint32_t start_symbol = hdr->cmnhdr.field.startSymbolId;
  int num_symbols = section->hdr.u.s1.numSymbol;
  int num_symbols_per_frame = NR_NUMBER_OF_SUBFRAMES_PER_FRAME * (1 << numerology) * NR_SYMBOLS_PER_SLOT;
  uint64_t symbol_in_frame = slot_in_frame * 14 + start_symbol;
  uint32_t current_symbol_in_frame = ctx->current_absolute_symbol % num_symbols_per_frame;
  int32_t diff = symbol_in_frame - current_symbol_in_frame;
  if (diff < -num_symbols_per_frame / 2) {
    diff += num_symbols_per_frame;
  } else if (diff > num_symbols_per_frame / 2) {
    diff -= num_symbols_per_frame;
  }
  if (diff > (int32_t)ctx->T2a_max_cp_dl_sym_diff) {
    ctx->stats.cplane_err_early++;
    return;
  }
  if (diff < (int32_t)ctx->T2a_min_cp_dl_sym_diff) {
    ctx->stats.cplane_err_late++;
    return;
  }
  uint64_t target_absolute_symbol = ctx->current_absolute_symbol + diff;
  bool is_dl_symbol = test_bit(ctx->dl_symbol_bitmask, target_absolute_symbol % ctx->symbol_bitmask_length);
  if (!is_dl_symbol) {
    ctx->stats.dl_tdd_mismatch++;
    return;
  }
  for (int i = 0; i < num_symbols; i++) {
    uint32_t job_index = (target_absolute_symbol + i) % MAX_CONCURRENT_SYMBOL_WINDOWS;
    dl_symbol_job_t *job = &ctx->dl_symbol_jobs[job_index];
    if (job->state == SYM_DL_IDLE) {
      job->absolute_symbol = target_absolute_symbol + i;
      job->state = SYM_DL_ACTIVE;
      job->expected_iq = 0;
      job->received_iq = 0;
      for (int j = 0; j < MAX_ANTENNAS; j++) {
        job->per_antenna[j].cplane_received = false;
        job->per_antenna[j].num_rx_fragments = 0;
        for (int k = 0; k < 4; k++) {
          job->per_antenna[j].rx_fragments[k].iq_data = NULL;
          job->per_antenna[j].rx_fragments[k].mbuf = NULL;
        }
      }
    } else if (job->state == SYM_DL_READY) {
      ctx->stats.application_too_slow++;
      return;
    }
    if (job->per_antenna[ant_id].cplane_received) {
      ctx->stats.cplane_err_dup++;
      return;
    }
    job->absolute_symbol = target_absolute_symbol + i;
    if (!job->per_antenna[ant_id].cplane_received) {
      job->per_antenna[ant_id].cplane_received = true;
      job->per_antenna[ant_id].num_rx_fragments = 0;
      for (int k = 0; k < 4; k++) {
        job->per_antenna[ant_id].rx_fragments[k].iq_data = NULL;
        job->per_antenna[ant_id].rx_fragments[k].mbuf = NULL;
      }
      job->per_antenna[ant_id].section_id = section->hdr.u1.common.sectionId;
      job->expected_iq += section->hdr.u1.common.numPrbc == 0 ? ctx->num_prb : section->hdr.u1.common.numPrbc;
    }
  }
}

static void handle_ul_cplane_packet(oru_packet_processor_context_t *ctx,
                                    void *pkt,
                                    struct xran_cp_radioapp_section1_header *hdr,
                                    struct xran_cp_radioapp_section1 *section,
                                    int ant_id)
{
  int numerology = ctx->numerology;
  int slot_in_frame = hdr->cmnhdr.field.slotId + hdr->cmnhdr.field.subframeId * (1 << numerology);
  uint32_t start_symbol = hdr->cmnhdr.field.startSymbolId;
  int num_symbols = section->hdr.u.s1.numSymbol;
  int num_symbols_per_frame = NR_NUMBER_OF_SUBFRAMES_PER_FRAME * (1 << numerology) * NR_SYMBOLS_PER_SLOT;
  uint64_t symbol_in_frame = slot_in_frame * 14 + start_symbol;
  uint32_t current_symbol_in_frame = ctx->current_absolute_symbol % num_symbols_per_frame;
  int32_t diff = symbol_in_frame - current_symbol_in_frame;
  if (diff < -num_symbols_per_frame / 2) {
    diff += num_symbols_per_frame;
  } else if (diff > num_symbols_per_frame / 2) {
    diff -= num_symbols_per_frame;
  }
  if (diff > (int32_t)ctx->T2a_max_cp_dl_sym_diff) {
    ctx->stats.cplane_err_early++;
    return;
  }
  if (diff < (int32_t)ctx->T2a_min_cp_dl_sym_diff) {
    ctx->stats.cplane_err_late++;
    return;
  }
  uint64_t target_absolute_symbol = ctx->current_absolute_symbol + diff;
  bool is_ul_symbol = test_bit(ctx->ul_symbol_bitmask, target_absolute_symbol % ctx->symbol_bitmask_length);
  if (!is_ul_symbol) {
    ctx->stats.ul_tdd_mismatch++;
    return;
  }
  for (int i = 0; i < num_symbols; i++) {
    uint32_t job_index = (target_absolute_symbol + i) % MAX_CONCURRENT_SYMBOL_WINDOWS;
    ul_symbol_job_t *job = &ctx->ul_symbol_jobs[job_index];
    if (job->state == SYM_UL_IDLE) {
      job->absolute_symbol = target_absolute_symbol + i;
      job->state = SYM_UL_ACTIVE;
      for (int j = 0; j < MAX_ANTENNAS; j++) {
        job->per_antenna[j].cplane_received = false;
      }
    } else if (job->state == SYM_UL_READY) {
      ctx->stats.application_too_slow++;
      return;
    }
    if (job->per_antenna[ant_id].cplane_received) {
      ctx->stats.cplane_err_dup++;
      return;
    }
    job->absolute_symbol = target_absolute_symbol + i;
    if (!job->per_antenna[ant_id].cplane_received) {
      job->per_antenna[ant_id].cplane_received = true;
      job->per_antenna[ant_id].section_id = section->hdr.u1.common.sectionId;
      job->per_antenna[ant_id].num_prb = section->hdr.u1.common.numPrbc == 0 ? ctx->num_prb : section->hdr.u1.common.numPrbc;
      job->per_antenna[ant_id].start_prb = section->hdr.u1.common.startPrbc;
    }
  }
}

void handle_cplane_packet(void *context, void *pkt)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  struct xran_ecpri_hdr *ecpri_hdr;
  struct xran_recv_packet_info xran_recv_packet_info;
  int ret = xran_parse_ecpri_hdr(pkt, &ecpri_hdr, &xran_recv_packet_info);
  if (ret != 0) {
    rte_pktmbuf_free(pkt);
    return;
  }
  uint8_t cu_port_id, band_sector_id, cc_id, ant_id;
  xran_decompose_cid(ecpri_hdr->ecpri_xtc_id, &ctx->eaxcid_config, &cu_port_id, &band_sector_id, &cc_id, &ant_id);
  ctx->stats.total_cplane++;
  struct xran_cp_radioapp_common_header *apphdr = (void *)rte_pktmbuf_adj(pkt, sizeof(struct xran_ecpri_hdr));
  if (apphdr == NULL) {
    ctx->stats.cplane_err_hdr++;
    rte_pktmbuf_free(pkt);
    return;
  }
  apphdr->field.all_bits = rte_be_to_cpu_32(apphdr->field.all_bits);
  if (apphdr->field.payloadVer != XRAN_PAYLOAD_VER) {
    ctx->stats.cplane_err_ver++;
    rte_pktmbuf_free(pkt);
    return;
  }

  switch (apphdr->sectionType) {
    case XRAN_CP_SECTIONTYPE_1: {
      struct xran_cp_radioapp_section1_header *hdr = (struct xran_cp_radioapp_section1_header *)apphdr;
      struct xran_cp_radioapp_section1 *section = (void *)rte_pktmbuf_adj(pkt, sizeof(struct xran_cp_radioapp_section1_header));
      if (section == NULL) {
        ctx->stats.cplane_err_hdr++;
        rte_pktmbuf_free(pkt);
        return;
      }
      *((uint64_t *)section) = rte_be_to_cpu_64(*((uint64_t *)section));
      if (hdr->cmnhdr.field.dataDirection == XRAN_DIR_DL) {
        handle_dl_cplane_packet(ctx, pkt, hdr, section, ant_id);
      } else {
        handle_ul_cplane_packet(ctx, pkt, hdr, section, ant_id);
      }

      rte_pktmbuf_free(pkt);
      return;
    }
    default:
      rte_pktmbuf_free(pkt);
      return;
  }
}

void print_packet_processor_stats(void *context)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx == NULL)
    return;

  printf("ORU Packet Processor Stats:\n");
  printf("  Total C-Plane Packets received: %lu\n", ctx->stats.total_cplane);
  printf("  Total U-Plane Packets received: %lu\n", ctx->stats.total_uplane_received);
  printf("  Total U-Plane Packets sent: %lu\n", ctx->thread_safe_stats.total_uplane_sent);

  if (ctx->stats.cplane_err_hdr > 0)
    printf("  C-Plane Header Errors: %lu\n", ctx->stats.cplane_err_hdr);
  if (ctx->stats.cplane_err_ver > 0)
    printf("  C-Plane Protocol Version Errors: %lu\n", ctx->stats.cplane_err_ver);
  if (ctx->stats.cplane_err_early > 0)
    printf("  C-Plane Timing Early Errors: %lu\n", ctx->stats.cplane_err_early);
  if (ctx->stats.cplane_err_late > 0)
    printf("  C-Plane Timing Late Errors: %lu\n", ctx->stats.cplane_err_late);
  if (ctx->stats.cplane_err_dup > 0)
    printf("  C-Plane Duplicate Packet Errors: %lu\n", ctx->stats.cplane_err_dup);
  if (ctx->stats.uplane_err_early > 0)
    printf("  U-Plane Timing Early Errors: %lu\n", ctx->stats.uplane_err_early);
  if (ctx->stats.uplane_err_late > 0)
    printf("  U-Plane Timing Late Errors: %lu\n", ctx->stats.uplane_err_late);
  if (ctx->stats.uplane_err_dup > 0)
    printf("  U-Plane Duplicate Packet Errors: %lu\n", ctx->stats.uplane_err_dup);
  if (ctx->stats.uplane_missing_cplane > 0)
    printf("  U-Plane Missing C-Plane Errors: %lu\n", ctx->stats.uplane_missing_cplane);
  if (ctx->stats.dl_tdd_mismatch + ctx->thread_safe_stats.dl_tdd_mismatch > 0)
    printf("  DL TDD Mismatch Errors: %lu\n", ctx->stats.dl_tdd_mismatch + ctx->thread_safe_stats.dl_tdd_mismatch);
  if (ctx->stats.ul_tdd_mismatch + ctx->thread_safe_stats.ul_tdd_mismatch > 0)
    printf("  UL TDD Mismatch Errors: %lu\n", ctx->stats.ul_tdd_mismatch + ctx->thread_safe_stats.ul_tdd_mismatch);
  if (ctx->stats.ul_cplane_missing + ctx->thread_safe_stats.ul_cplane_missing > 0)
    printf("  UL C-Plane Missing Errors: %lu\n", ctx->stats.ul_cplane_missing + ctx->thread_safe_stats.ul_cplane_missing);
  if (ctx->stats.out_of_mbufs + ctx->thread_safe_stats.out_of_mbufs > 0)
    printf("  Out Of Mbufs Errors: %lu\n", ctx->stats.out_of_mbufs + ctx->thread_safe_stats.out_of_mbufs);
  if (ctx->stats.application_too_slow > 0)
    printf("  Application Too Slow Errors: %lu\n", ctx->stats.application_too_slow);

  memset(&ctx->stats, 0, sizeof(ctx->stats));
}

void get_packet_processor_stats(void *context, oru_packet_processor_stats_t *out_stats)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx && out_stats) {
    *out_stats = ctx->stats;
    out_stats->dl_tdd_mismatch += ctx->thread_safe_stats.dl_tdd_mismatch;
    out_stats->ul_tdd_mismatch += ctx->thread_safe_stats.ul_tdd_mismatch;
    out_stats->ul_cplane_missing += ctx->thread_safe_stats.ul_cplane_missing;
    out_stats->out_of_mbufs += ctx->thread_safe_stats.out_of_mbufs;
    out_stats->total_uplane_sent = ctx->thread_safe_stats.total_uplane_sent;
  }
}

static void unpack_iq(c16_t *txdataF, void *iqdata, int start_prb, int num_prb)
{
  uint32_t *source = (uint32_t *)iqdata;
  uint32_t *destination = (uint32_t *)&txdataF[start_prb * NR_NB_SC_PER_RB];
  for (int j = 0; j < num_prb * NR_NB_SC_PER_RB; j++) {
    destination[j] = rte_bswap32(source[j]);
  }
}

void read_dl_iq(void *context, uint32_t **txdataF, int nb_tx, int *frame, int *slot, int *symbol)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx == NULL)
    return;
  while (rte_ring_count(ctx->dl_ready_jobs) == 0)
    rte_pause();
  uintptr_t job_index;
  int ret = rte_ring_dequeue(ctx->dl_ready_jobs, (void **)&job_index);
  AssertFatal(ret == 0, "Failed to dequeue from ring dl_ready_jobs\n");
  dl_symbol_job_t *job = &ctx->dl_symbol_jobs[job_index];
  AssertFatal(job->state == SYM_DL_READY || job->state == SYM_DL_EMPTY, "Incorrect job state (%d)\n", job->state);
  uint64_t absolute_gps_symbol = job->absolute_symbol;
  int numerology = ctx->numerology;
  int num_symbols_per_frame = NR_NUMBER_OF_SUBFRAMES_PER_FRAME * (1 << numerology) * NR_SYMBOLS_PER_SLOT;
  *frame = (absolute_gps_symbol / num_symbols_per_frame) % 1024;
  *slot = absolute_gps_symbol % num_symbols_per_frame;
  *symbol = absolute_gps_symbol % NR_SYMBOLS_PER_SLOT;

  for (int aatx = 0; aatx < nb_tx; aatx++) {
    memset(txdataF[aatx], 0, ctx->num_prb * NR_NB_SC_PER_RB * sizeof(uint32_t));
    if (job->state == SYM_DL_EMPTY || job->per_antenna[aatx].num_rx_fragments == 0) {
      continue;
    }
    for (int k = 0; k < job->per_antenna[aatx].num_rx_fragments; k++) {
      unpack_iq((c16_t *)txdataF[aatx],
                job->per_antenna[aatx].rx_fragments[k].iq_data,
                job->per_antenna[aatx].rx_fragments[k].start_prbc,
                job->per_antenna[aatx].rx_fragments[k].num_prbc);
      if (job->per_antenna[aatx].rx_fragments[k].mbuf) {
        rte_pktmbuf_free(job->per_antenna[aatx].rx_fragments[k].mbuf);
      }
    }
  }
  for (int aatx = 0; aatx < MAX_ANTENNAS; aatx++) {
    job->per_antenna[aatx].num_rx_fragments = 0;
    job->per_antenna[aatx].cplane_received = false;
  }
  job->state = SYM_DL_IDLE;
}

int get_ready_job_count(void *context)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx == NULL)
    return 0;
  return rte_ring_count(ctx->dl_ready_jobs);
}

void fill_ecpri_header(struct xran_ecpri_hdr *ecpri_header,
                       struct xran_eaxcid_config *eaxcid_config,
                       uint8_t ecpri_mesg_type,
                       size_t ecpri_payload_size,
                       uint8_t CC_ID,
                       uint8_t Ant_ID,
                       uint8_t seq_id,
                       uint8_t oxu_port_id)
{
  ecpri_header->cmnhdr.data.data_num_1 = 0x0;
  ecpri_header->cmnhdr.bits.ecpri_ver = XRAN_ECPRI_VER;
  ecpri_header->cmnhdr.bits.ecpri_mesg_type = ecpri_mesg_type;
  ecpri_header->cmnhdr.bits.ecpri_payl_size = rte_cpu_to_be_16(ecpri_payload_size);
  ecpri_header->ecpri_xtc_id = xran_compose_cid(eaxcid_config, 0, 0, CC_ID, Ant_ID);
  ecpri_header->ecpri_seq_id.bits.seq_id = seq_id;
  ecpri_header->ecpri_seq_id.bits.e_bit = 1;
  ecpri_header->ecpri_seq_id.bits.sub_seq_id = 0;
  /// No byteswap for ecpri_seq_id. Possibly because of inverse definition in xran
}

void fill_radio_app_header(struct radio_app_common_hdr *radio_app_header,
                           int filter_id,
                           int direction,
                           int frame,
                           int slot,
                           int symbol,
                           int mu)
{
  radio_app_header->frame_id = frame & 0xff;
  radio_app_header->sf_slot_sym.slot_id = slot % (1 << mu);
  radio_app_header->sf_slot_sym.subframe_id = slot / (1 << mu);
  radio_app_header->sf_slot_sym.symb_id = symbol;
  radio_app_header->sf_slot_sym.value = rte_cpu_to_be_16(radio_app_header->sf_slot_sym.value);
  radio_app_header->data_feature.data_direction = direction;
  radio_app_header->data_feature.payl_ver = 1;
  radio_app_header->data_feature.filter_id = filter_id;
}

void fill_data_section_header(struct data_section_hdr *data_section_hdr, int num_prb, int start_prb, int section_id)
{
  data_section_hdr->fields.all_bits = 0;
  data_section_hdr->fields.num_prbu = (uint8_t)XRAN_CONVERT_NUMPRBC(num_prb);
  data_section_hdr->fields.start_prbu = (start_prb & 0x03ff);
  data_section_hdr->fields.sect_id = section_id;
  data_section_hdr->fields.all_bits = rte_cpu_to_be_32(data_section_hdr->fields.all_bits);
}

void write_ul_iq(void *context, uint32_t **txdataF, int nb_rx, int frame, int slot_in_frame, int symbol)
{
  oru_packet_processor_context_t *ctx = (oru_packet_processor_context_t *)context;
  if (ctx == NULL)
    return;
  int numerology = ctx->numerology;
  int num_symbols_per_frame = NR_NUMBER_OF_SUBFRAMES_PER_FRAME * (1 << numerology) * NR_SYMBOLS_PER_SLOT;
  uint32_t current_symbol_in_frame = ctx->current_absolute_symbol % num_symbols_per_frame;
  int symbol_in_frame = NR_SYMBOLS_PER_SLOT * slot_in_frame + symbol;
  int32_t diff = symbol_in_frame - current_symbol_in_frame;
  if (diff < -num_symbols_per_frame / 2) {
    diff += num_symbols_per_frame;
  } else if (diff > num_symbols_per_frame / 2) {
    diff -= num_symbols_per_frame;
  }
  uint64_t target_absolute_symbol = ctx->current_absolute_symbol + diff;
  bool is_ul_symbol = test_bit(ctx->ul_symbol_bitmask, target_absolute_symbol % ctx->symbol_bitmask_length);
  if (!is_ul_symbol) {
    ctx->thread_safe_stats.ul_tdd_mismatch++;
    return;
  }
  ul_symbol_job_t *job = &ctx->ul_symbol_jobs[target_absolute_symbol % MAX_CONCURRENT_SYMBOL_WINDOWS];
  if (job->state == SYM_UL_IDLE) {
    ctx->thread_safe_stats.ul_cplane_missing++;
    return;
  }
  int mu = ctx->numerology;
  struct rte_mbuf *mbufs[MAX_MBUFS_PER_SYMBOL];
  uint32_t num_mbufs = 0;
  for (int aarx = 0; aarx < nb_rx; aarx++) {
    if (!job->per_antenna[aarx].cplane_received) {
      ctx->thread_safe_stats.ul_cplane_missing++;
      continue;
    }
    int section_id = job->per_antenna[aarx].section_id;
    int total_ul_rbs = job->per_antenna[aarx].num_prb;
    int start_prb_base = job->per_antenna[aarx].start_prb;

    int rbs_sent = 0;
    size_t overhead = sizeof(struct rte_ether_hdr) + sizeof(struct xran_ecpri_hdr) + sizeof(struct radio_app_common_hdr)
                      + sizeof(struct data_section_hdr) + sizeof(struct data_section_compression_hdr);
    int max_prb_per_packet = (int)((ctx->mtu - overhead) / (NR_NB_SC_PER_RB * sizeof(int32_t)));

    while (rbs_sent < total_ul_rbs) {
      int num_ul_rbs = total_ul_rbs - rbs_sent;
      if (num_ul_rbs > max_prb_per_packet) {
        num_ul_rbs = max_prb_per_packet;
      }

      struct rte_mbuf *pkt = ctx->alloc_func(ctx->io_controller);
      if (pkt == NULL) {
        ctx->thread_safe_stats.out_of_mbufs++;
        break;
      }
      
      size_t header_length = sizeof(struct xran_ecpri_hdr) + sizeof(struct radio_app_common_hdr) + sizeof(struct data_section_hdr)
                             + sizeof(struct data_section_compression_hdr);
      const uint num_sc = num_ul_rbs * NR_NB_SC_PER_RB;
      size_t data_len = sizeof(int32_t) * num_sc;
      
      char *buf = rte_pktmbuf_append(pkt, (uint16_t)(header_length + data_len));
      if (buf == NULL) {
        LOG_W(HW, "ORU: Failed to append data to mbuf (insufficient space)\n");
        rte_pktmbuf_free(pkt);
        // Do not increment rbs_sent, just break and let the next symbol or antenna be processed,
        // or we could reduce max_prb_per_packet. Breaking is safer to prevent infinite loop.
        break;
      }

      if (num_mbufs == (MAX_MBUFS_PER_SYMBOL - 1)) {
        ctx->send_func(ctx->io_controller, mbufs, num_mbufs);
        ctx->thread_safe_stats.total_uplane_sent += num_mbufs;
        num_mbufs = 0;
      }
      mbufs[num_mbufs++] = pkt;

      struct xran_ecpri_hdr *ecpri_header = (struct xran_ecpri_hdr *)buf;
      // eCPRI payload size excludes the 4-byte common header
      uint16_t ecpri_payload_size = (uint16_t)(header_length - 4 + data_len);
      fill_ecpri_header(ecpri_header,
                        &ctx->eaxcid_config,
                        ECPRI_IQ_DATA,
                        ecpri_payload_size,
                        0,
                        aarx,
                        ctx->pusch_seq_id[aarx]++,
                        0);

      struct radio_app_common_hdr *radio_app_header = (struct radio_app_common_hdr *)(ecpri_header + 1);
      fill_radio_app_header(radio_app_header, 0, XRAN_DIR_UL, frame, slot_in_frame, symbol, mu);

      struct data_section_hdr *data_section_header = (struct data_section_hdr *)(radio_app_header + 1);
      fill_data_section_header(data_section_header, num_ul_rbs, start_prb_base + rbs_sent, section_id);

      struct data_section_compression_hdr *compression_header = (struct data_section_compression_hdr *)(data_section_header + 1);
      compression_header->ud_comp_hdr.ud_comp_meth = 0;
      compression_header->ud_comp_hdr.ud_iq_width = XRAN_CONVERT_IQWIDTH(16);
      compression_header->rsrvd = 0;

      void *iq_data_start = (void *)(compression_header + 1);
      uint32_t *src = &txdataF[aarx][(start_prb_base + rbs_sent) * NR_NB_SC_PER_RB];
      uint32_t *dst = iq_data_start;
      for (int i = 0; i < num_sc; i++) {
        *dst++ = rte_cpu_to_be_32(*src++);
      }
      rbs_sent += num_ul_rbs;
    }
  }
  ctx->send_func(ctx->io_controller, mbufs, num_mbufs);
  ctx->thread_safe_stats.total_uplane_sent += num_mbufs;
  for (int i = 0; i < MAX_ANTENNAS; i++) {
    job->per_antenna[i].cplane_received = false;
  }
  job->state = SYM_UL_IDLE;
}
