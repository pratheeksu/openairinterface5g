/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file PHY/NR_ESTIMATION/nr_measurements_gNB.c
* \brief gNB measurement routines
* \author Ahmed Hussein, G. Casati, K. Saaifan
* \date 2019
* \version 0.1
* \company Fraunhofer IIS
* \email: ahmed.hussein@iis.fraunhofer.de, guido.casati@iis.fraunhofer.de, khodr.saaifan@iis.fraunhofer.de
* \note
* \warning
*/

#include "PHY/types.h"
#include "PHY/defs_gNB.h"
#include "nr_ul_estimation.h"

#define I0_SKIP_DC 1

void nr_est_srs_timing_advance_offset(uint16_t ofdm_symbol_size,
                                      const c16_t srs_estimated_channel_time[][NR_SRS_IDFT_OVERSAMP_FACTOR * ofdm_symbol_size],
                                      uint8_t ant,
                                      uint8_t N_ap,
                                      uint32_t samples_per_frame,
                                      uint16_t *ta_offset,
                                      int16_t *ta_offset_nsec)
{
  int64_t mean_val = 0;
  int64_t max_val = 0;
  int32_t max_idx = 0;
  int16_t srs_toa = 0;
  int16_t ofdm_os_size = NR_SRS_IDFT_OVERSAMP_FACTOR * ofdm_symbol_size;
  for (int k = 0; k < ofdm_os_size; k++) {
    int64_t abs_val = 0;
    for (int p_index = 0; p_index < N_ap; p_index++) {
      abs_val += squaredMod(srs_estimated_channel_time[p_index][k]);
    }
    mean_val += abs_val;
    if (abs_val > max_val) {
      max_val = abs_val;
      max_idx = k;
    }
  }
  max_val = max_val / N_ap;
  mean_val = mean_val / (N_ap * ofdm_os_size);

  if (max_idx > ofdm_os_size >> 1)
    max_idx = max_idx - ofdm_os_size;

  // Check for detection threshold
  if ((mean_val != 0) && (max_val / mean_val > NR_SRS_DETECTION_THRESHOLD)) {
    srs_toa = max_idx / NR_SRS_IDFT_OVERSAMP_FACTOR;

    // restrict the computation of ta_offset to antenna 0
    if (ant == 0) {
      // Scale the 16 factor in N_TA calculation in 38.213 section 4.2 according to the used FFT size
      const uint16_t bw_scaling = ofdm_symbol_size >> 7;

      // do some integer rounding to improve TA accuracy
      int sync_pos_rounded;
      if (srs_toa > 0) {
        sync_pos_rounded = srs_toa + (bw_scaling >> 1) - 1;
      } else {
        sync_pos_rounded = srs_toa - (bw_scaling >> 1) + 1;
      }

      *ta_offset = sync_pos_rounded / bw_scaling;

      // put timing advance command in 0..63 range
      *ta_offset = BOUNDED_EVAL(0, *ta_offset + 31, 63);
    }
    *ta_offset_nsec = (max_idx * 1e9) / (NR_SRS_IDFT_OVERSAMP_FACTOR * samples_per_frame * 100);
  } else {
    *ta_offset = 0xFFFF;
    *ta_offset_nsec = 0x8000;
  }

  LOG_D(NR_PHY,
        "SRS estimatd ToA %d [RX ant %d]: TA offset %d, TA offset ns %d (max_val %ld, mean_val %ld, max_idx %d)\n",
        srs_toa,
        ant,
        *ta_offset,
        *ta_offset_nsec,
        max_val,
        mean_val,
        max_idx);
}

void dump_nr_I0_stats(FILE *fd,PHY_VARS_gNB *gNB) {
  int min_I0 = 1000, max_I0 = 0;
  int amin = 0, amax = 0;
  fprintf(fd, "Blacklisted PRBs %d/%d\n", gNB->num_ulprbbl, gNB->frame_parms.N_RB_UL);
  for (int i = 0; i < gNB->frame_parms.N_RB_UL; i++) {
    if (gNB->ulprbbl[i] > 0)
      continue;
    if (gNB->measurements.n0_subband_power_tot_dB[i] < min_I0) {
      min_I0 = gNB->measurements.n0_subband_power_tot_dB[i];
      amin = i;
    }
    if (gNB->measurements.n0_subband_power_tot_dB[i] > max_I0) {
      max_I0 = gNB->measurements.n0_subband_power_tot_dB[i];
      amax = i;
    }
  }

  for (int i = 0; i < gNB->frame_parms.N_RB_UL; i++) {
    if (gNB->ulprbbl[i] == 0)
      fprintf(fd, "%2d.", gNB->measurements.n0_subband_power_tot_dB[i] - gNB->measurements.n0_subband_power_avg_dB);
    else
      fprintf(fd, " X.");
    if (i % 25 == 24)
      fprintf(fd, "\n");
  }
  fprintf(fd, "\n");
  fprintf(fd,
          "max_IO = %d (%d), min_I0 = %d (%d), avg_I0 = %d dB",
          max_I0,
          amax,
          min_I0,
          amin,
          gNB->measurements.n0_subband_power_avg_dB);
  if (gNB->frame_parms.nb_antennas_rx > 1) {
    fprintf(fd, "(");
    for (int aarx = 0; aarx < gNB->frame_parms.nb_antennas_rx; aarx++)
      fprintf(fd, "%d.", gNB->measurements.n0_subband_power_avg_perANT_dB[aarx]);
    fprintf(fd, ")");
  }
  fprintf(fd, "\nPRACH I0 = %d.%d dB\n", gNB->measurements.prach_I0 / 10, gNB->measurements.prach_I0 % 10);
}

// Function to fill UL RB mask to be used for N0 measurements
static void fill_ul_rb_mask(PHY_VARS_gNB *gNB,
                            int frame_rx,
                            int slot_rx,
                            uint8_t rb_mask_ul[14][275],
                            nfapi_nr_max_num_of_symbol_per_slot_t *slot_conf)
{
  for (int symbol = 0; symbol < 14; symbol++)
    for (int prbpos = 0; prbpos < 275; prbpos++)
      rb_mask_ul[symbol][prbpos] = gNB->ulprbbl[prbpos];

  for (int i = 0; i < gNB->max_nb_pucch; i++) {
    const NR_gNB_PUCCH_t *pucch = gNB->pucch + i;
    if (!(pucch && pucch->active && pucch->frame == frame_rx && pucch->slot == slot_rx))
      continue;
    const nfapi_nr_pucch_pdu_t *pucch_pdu = &pucch->pucch_pdu;
    const int start = pucch_pdu->start_symbol_index;
    LOG_D(PHY,
          "%d.%d pucch %d : start_symbol %d, nb_symbols %d, prb_size %d\n",
          frame_rx,
          slot_rx,
          i,
          start,
          pucch_pdu->nr_of_symbols,
          pucch_pdu->prb_size);
    for (int symbol = start; symbol < (start + pucch_pdu->nr_of_symbols); symbol++) {
      if (gNB->frame_parms.frame_type == FDD || (gNB->frame_parms.frame_type == TDD && slot_conf[symbol].slot_config.value == 1)) {
        for (int rb = 0; rb < pucch_pdu->prb_size; rb++) {
          int rb2 =
              rb + pucch_pdu->bwp_start
              + ((symbol < start + (pucch_pdu->nr_of_symbols >> 1)) || (pucch_pdu->freq_hop_flag == 0) ? pucch_pdu->prb_start
                                                                                                       : pucch_pdu->second_hop_prb);
          rb_mask_ul[symbol][rb2] = 1;
        }
      }
    }
  }

  for (int ULSCH_id = 0; ULSCH_id < gNB->max_nb_pusch; ULSCH_id++) {
    NR_gNB_ULSCH_t *ulsch = &gNB->ulsch[ULSCH_id];
    NR_UL_gNB_HARQ_t *ulsch_harq = ulsch->harq_process;
    AssertFatal(ulsch_harq != NULL, "harq_pid %d is not allocated\n", ulsch->harq_pid);
    if (!(ulsch->active && ulsch->frame == frame_rx && ulsch->slot == slot_rx && !ulsch->handled))
      continue;
    uint8_t symbol_start = ulsch_harq->ulsch_pdu.start_symbol_index;
    uint8_t symbol_end = symbol_start + ulsch_harq->ulsch_pdu.nr_of_symbols;
    for (int symbol = symbol_start; symbol < symbol_end; symbol++) {
      if (gNB->frame_parms.frame_type == FDD || (gNB->frame_parms.frame_type == TDD && slot_conf[symbol].slot_config.value == 1)) {
        LOG_D(PHY, "symbol %d Filling rb_mask_ul rb_size %d\n", symbol, ulsch_harq->ulsch_pdu.rb_size);
        for (int rb = 0; rb < ulsch_harq->ulsch_pdu.rb_size; rb++) {
          int rb2 = rb + ulsch_harq->ulsch_pdu.rb_start + ulsch_harq->ulsch_pdu.bwp_start;
          rb_mask_ul[symbol][rb2] = 1;
        }
      }
    }
  }

  for (int i = 0; i < gNB->max_nb_srs; i++) {
    NR_gNB_SRS_t *srs = &gNB->srs[i];
    if (!(srs && srs->active && srs->frame == frame_rx && srs->slot == slot_rx))
      continue;
    nfapi_nr_srs_pdu_t *srs_pdu = &srs->srs_pdu;
    const uint8_t l0 = gNB->frame_parms.symbols_per_slot - 1 - srs_pdu->time_start_position;
    for (int symbol = 0; symbol < (1 << srs_pdu->num_symbols); symbol++) {
      for (int rb = srs_pdu->bwp_start; rb < (srs_pdu->bwp_start + srs_pdu->bwp_size); rb++) {
        rb_mask_ul[l0 + symbol][rb] = 1;
      }
    }
  }
}

void gNB_I0_measurements(PHY_VARS_gNB *gNB, int frame_rx, int slot, uint8_t rb_mask_set[14][275])
{
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  NR_gNB_COMMON *common_vars = &gNB->common_vars;
  PHY_MEASUREMENTS_gNB *measurements = &gNB->measurements;

  int buzy_symb[275] = {0};
  // Mask of occupied RBs, per symbol and PRB
  nfapi_nr_max_num_of_symbol_per_slot_t *slot_conf = NULL;

  if (frame_parms->frame_type == TDD)
    slot_conf = gNB->gNB_config.tdd_table.max_tdd_periodicity_list[slot].max_num_of_symbol_per_slot_list;

  int num_symb = 0;
  int first_symb = 0;
  if (frame_parms->frame_type == TDD)
    for (int symbol_count = 0; symbol_count < NR_NUMBER_OF_SYMBOLS_PER_SLOT; symbol_count++) {
      if (slot_conf[symbol_count].slot_config.value == 1) {
        if (num_symb == 0)
          first_symb = symbol_count;
        num_symb++;
      }
    }
  else
    num_symb = NR_NUMBER_OF_SYMBOLS_PER_SLOT;

  uint8_t rb_mask_ul[14][275];
  if (rb_mask_set)
    memcpy(rb_mask_ul, rb_mask_set, sizeof(rb_mask_ul));
  else
    fill_ul_rb_mask(gNB, frame_rx, slot, rb_mask_ul, slot_conf);
  unsigned int tmp_n0_subband[frame_parms->nb_antennas_rx][frame_parms->N_RB_UL];
  memset(tmp_n0_subband, 0, sizeof(tmp_n0_subband));

  // TODO modify the measurements to take into account concurrent beams
  for (int symb = first_symb; symb < first_symb + num_symb; symb++) {
    int offset0 = ((slot % RU_RX_SLOT_DEPTH) * frame_parms->symbols_per_slot + symb) * frame_parms->ofdm_symbol_size;
    for (int rb = 0; rb < frame_parms->N_RB_UL; rb++) {
      if (rb_mask_ul[symb][rb] == 0 && // check that rb was not used in this subframe
          !(I0_SKIP_DC && rb == frame_parms->N_RB_UL >> 1)) { // skip middle PRB because of artificial noise possibly created by FFT
        int offset = offset0 + (frame_parms->first_carrier_offset + (rb * 12)) % frame_parms->ofdm_symbol_size;
        buzy_symb[rb]++;
        for (int aarx = 0; aarx < frame_parms->nb_antennas_rx; aarx++) {
          c16_t *ul_ch = &common_vars->rxdataF[0][aarx][offset];
          int32_t signal_energy;
          if (((frame_parms->N_RB_UL & 1) == 1) && (rb == (frame_parms->N_RB_UL >> 1))) {
            signal_energy = signal_energy_nodc(ul_ch, 6);
            ul_ch = &common_vars->rxdataF[0][aarx][offset0];
            signal_energy += signal_energy_nodc(ul_ch, 6);
          } else {
            signal_energy = signal_energy_nodc(ul_ch, 12);
          }
          // noise power per antenna/RB, symbols summed up
          tmp_n0_subband[aarx][rb] += signal_energy;
          LOG_D(NR_PHY, "slot %d symbol %d RB %d aarx %d n0_subband_power %d\n", slot, symb, rb, aarx, signal_energy);
        } // antenna
      }
    } // rb
  } // symb
  int nb_rb = 0;
  int32_t n0_subband_tot_perANT[frame_parms->nb_antennas_rx];
  memset(n0_subband_tot_perANT, 0, sizeof(n0_subband_tot_perANT));

  allocCast2D(n0_subband_power,
              unsigned int,
              measurements->n0_subband_power,
              frame_parms->nb_antennas_rx,
              frame_parms->N_RB_UL,
              false);
  for (int rb = 0; rb < frame_parms->N_RB_UL; rb++) {
    int32_t n0_subband_tot_perPRB = 0;
    if (buzy_symb[rb] > 0) {
      for (int aarx = 0; aarx < frame_parms->nb_antennas_rx; aarx++) {
        tmp_n0_subband[aarx][rb] /= buzy_symb[rb];
        // apply exponential moving average if noise on antenna/RB, otherwise
        // retain previous measurement
        if (tmp_n0_subband[aarx][rb] != 0 || IS_SOFTMODEM_RFSIM)
          n0_subband_power[aarx][rb] = 0.9 * n0_subband_power[aarx][rb] + 0.1 * tmp_n0_subband[aarx][rb];
        n0_subband_tot_perPRB += n0_subband_power[aarx][rb];
        n0_subband_tot_perANT[aarx] += n0_subband_power[aarx][rb];
      }
      n0_subband_tot_perPRB /= frame_parms->nb_antennas_rx;
      measurements->n0_subband_power_tot_dB[rb] = dB_fixed(n0_subband_tot_perPRB);
      LOG_D(NR_PHY,
            "n0_subband_power_tot_dB[%d] => %d, over %d symbols\n",
            rb,
            measurements->n0_subband_power_tot_dB[rb],
            buzy_symb[rb]);
      nb_rb++;
    }
  }
  if (nb_rb > 0) {
    int64_t n0_subband_tot = 0;
    for (int aarx = 0; aarx < frame_parms->nb_antennas_rx; aarx++) {
      measurements->n0_subband_power_avg_perANT_dB[aarx] = dB_fixed(n0_subband_tot_perANT[aarx] / nb_rb);
      n0_subband_tot += n0_subband_tot_perANT[aarx];
    }
    measurements->n0_subband_power_avg_dB = dB_fixed(n0_subband_tot / (frame_parms->nb_antennas_rx * nb_rb));
  }
}
