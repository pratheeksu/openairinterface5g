/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef __NR_COMPUTE_LLR__H__
#define __NR_COMPUTE_LLR__H__

#include "PHY/impl_defs_top.h"

/**
 * @brief Per-layer LLR dispatch for single-stream or post-MMSE output.
 *
 * Selects the appropriate single-layer LLR function based on modulation order.
 *
 * @param rxdataF_comp  Compensated received signal
 * @param ch_mag        Channel magnitude (threshold a), used for 16/64/256QAM
 * @param ch_magb       Channel magnitude (threshold b), used for 64/256QAM
 * @param ch_magc       Channel magnitude (threshold c), used for 256QAM
 * @param llr           Output LLR buffer
 * @param nb_re         Number of resource elements
 * @param symbol        OFDM symbol index (used only in AssertFatal message)
 * @param mod_order     Modulation order (2=QPSK, 4=16QAM, 6=64QAM, 8=256QAM)
 */
void nr_compute_llr(c16_t *rxdataF_comp,
                    c16_t *ch_mag,
                    c16_t *ch_magb,
                    c16_t *ch_magc,
                    int16_t *llr,
                    uint32_t nb_re,
                    uint8_t symbol,
                    uint8_t mod_order);

/**
 * @brief 2-layer ML-LLR for QPSK×QPSK.
 *
 * Computes LLRs for stream 0 in the presence of interference from stream 1,
 * both modulated with QPSK. Uses 128-bit SIMD on aarch64, 256-bit AVX2 elsewhere.
 *
 * @param stream0_in   MF output for stream 0: y0' = h0'·y0
 * @param stream1_in   MF output for stream 1: y1' = h1'·y0
 * @param stream0_out  Output LLRs for stream 0
 * @param rho01        Channel cross-correlation: rho01 = h0'·h1
 * @param length       Number of resource elements
 */
void nr_qpsk_llr_2layer(c16_t *stream0_in, c16_t *stream1_in,
                        int16_t *stream0_out, c16_t *rho01, uint32_t length);

/**
 * @brief 2-layer ML-LLR for 16QAM×16QAM.
 *
 * @param stream0_in   MF output for stream 0
 * @param stream1_in   MF output for stream 1
 * @param ch_mag       Channel magnitude for stream 0
 * @param ch_mag_i     Channel magnitude for stream 1
 * @param stream0_out  Output LLRs for stream 0
 * @param rho01        Channel cross-correlation
 * @param length       Number of resource elements
 */
void nr_qam16_llr_2layer(c16_t *stream0_in, c16_t *stream1_in,
                         c16_t *ch_mag, c16_t *ch_mag_i,
                         int16_t *stream0_out, c16_t *rho01, uint32_t length);

/**
 * @brief 2-layer ML-LLR for 64QAM×64QAM.
 *
 * @param stream0_in   MF output for stream 0
 * @param stream1_in   MF output for stream 1
 * @param ch_mag       Channel magnitude (threshold a) for stream 0
 * @param ch_mag_i     Channel magnitude (threshold a) for stream 1
 * @param stream0_out  Output LLRs for stream 0
 * @param rho01        Channel cross-correlation
 * @param length       Number of resource elements
 */
void nr_qam64_llr_2layer(c16_t *stream0_in, c16_t *stream1_in,
                         c16_t *ch_mag, c16_t *ch_mag_i,
                         int16_t *stream0_out, c16_t *rho01, uint32_t length);

/**
 * @brief Compute ML LLRs for both streams of a 2-layer MIMO transmission.
 *
 * Dispatches to nr_qpsk_llr_2layer / nr_qam16_llr_2layer / nr_qam64_llr_2layer
 * for each stream in sequence.
 *
 * @param rxdataF_comp0  Compensated received signal for layer 0
 * @param rxdataF_comp1  Compensated received signal for layer 1
 * @param ul_ch_mag0     Channel magnitude for layer 0
 * @param ul_ch_mag1     Channel magnitude for layer 1
 * @param llr_layers0    Output LLR buffer for layer 0
 * @param llr_layers1    Output LLR buffer for layer 1
 * @param rho0           Cross-correlation rho[0][1]
 * @param rho1           Cross-correlation rho[1][0]
 * @param nb_re          Number of resource elements
 * @param mod_order      Modulation order (2=QPSK, 4=16QAM, 6=64QAM)
 */
void nr_compute_ML_llr(c16_t *rxdataF_comp0, c16_t *rxdataF_comp1,
                       c16_t *ul_ch_mag0, c16_t *ul_ch_mag1,
                       int16_t *llr_layers0, int16_t *llr_layers1,
                       c16_t *rho0, c16_t *rho1,
                       uint32_t nb_re, uint8_t mod_order);

/**
 * @brief MMSE equalizer for 2-layer MIMO, followed by per-layer LLR computation.
 *
 * Performs 2×2 MMSE equalization, updates ch_mag arrays with post-equalization
 * effective SNR, and computes per-layer LLRs via nr_compute_llr.
 *
 * @param rxdataF_comp         Compensated received signal [nl * nb_rx_ant][buffer_length]
 * @param buffer_length        Samples per symbol
 * @param nb_rx_ant            Number of Rx antennas
 * @param ul_ch_mag            Channel magnitude threshold 'a' [2][buffer_length]
 * @param ul_ch_magb           Channel magnitude threshold 'b' [2][buffer_length]
 * @param ul_ch_magc           Channel magnitude threshold 'c' [2][buffer_length]
 * @param ul_ch_estimates_ext  Channel estimates [2][nb_rx_ant][buffer_length]
 * @param nb_rb                Number of allocated resource blocks
 * @param mod_order            Modulation order
 * @param shift                Right-shift for internal fixed-point arithmetic
 * @param symbol               OFDM symbol index
 * @param length               Number of resource elements to process
 * @param noise_var            Noise variance estimate
 * @return                     0 on success
 */
uint8_t nr_mmse_2layers(c16_t **rxdataF_comp,
                        uint32_t buffer_length,
                        int nb_rx_ant,
                        c16_t ul_ch_mag[][buffer_length],
                        c16_t ul_ch_magb[][buffer_length],
                        c16_t ul_ch_magc[][buffer_length],
                        c16_t ul_ch_estimates_ext[][nb_rx_ant][buffer_length],
                        unsigned short nb_rb,
                        unsigned char mod_order,
                        int shift,
                        unsigned char symbol,
                        int length,
                        uint32_t noise_var);

#endif /* __NR_COMPUTE_LLR__H__ */
