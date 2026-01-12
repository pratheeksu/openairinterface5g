/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "PHY/CODING/nrPolar_tools/polar_interface.h"
#include "common/utils/load_module_shlib.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/coding_defs.h"
#include "SIMULATION/TOOLS/sim.h"
#include "common/config/config_userapi.h"
// #include "common/utils/LOG/log.h"
#include "coding_unitary_defs.h"
// #define DEBUG_DCI_POLAR_PARAMS
// #define DEBUG_POLAR_TIMING
// #define DEBUG_POLARTEST
polar_interface_t polar_interface;

// =============================================
//         code / decode references
// =============================================
////////////////////////////////////////////////
////////////////////encoder/////////////////////
////////////////////////////////////////////////
void polar_encoder_legacy(uint32_t *in, uint32_t *out, int8_t messageType, uint16_t messageLength, uint8_t aggregation_level)
{
  t_nrPolar_params *polarParams = nr_polar_params(messageType, messageLength, aggregation_level);
  uint8_t nr_polar_A[polarParams->payloadBits];
  nr_bit2byte_uint32_8(in, polarParams->payloadBits, nr_polar_A);
  /*
   * Bytewise operations
   */
  // Calculate CRC.
  uint8_t nr_polar_crc[polarParams->crcParityBits];
  nr_matrix_multiplication_uint8_1D_uint8_2D(nr_polar_A,
                                             polarParams->crc_generator_matrix,
                                             nr_polar_crc,
                                             polarParams->payloadBits,
                                             polarParams->crcParityBits);

  for (uint i = 0; i < polarParams->crcParityBits; i++)
    nr_polar_crc[i] %= 2;

  uint8_t nr_polar_B[polarParams->K];
  // Attach CRC to the Transport Block. (a to b)
  memcpy(nr_polar_B, nr_polar_A, polarParams->payloadBits);
  for (uint i = polarParams->payloadBits; i < polarParams->K; i++)
    nr_polar_B[i] = nr_polar_crc[i - (polarParams->payloadBits)];

#ifdef DEBUG_POLAR_ENCODER
  uint64_t B2 = 0;

  for (int i = 0; i < polarParams->K; i++)
    B2 |= ((uint64_t)nr_polar_B[i] << i);

  printf("polar_B %lx\n", B2);
  for (int i = 0; i < polarParams->payloadBits; i++)
    printf("a[%d]=%d\n", i, nr_polar_A[i]);
  for (int i = 0; i < polarParams->K; i++)
    printf("b[%d]=%d\n", i, nr_polar_B[i]);
#endif

  // Interleaving (c to c')
  uint8_t nr_polar_CPrime[polarParams->K];
  nr_polar_interleaver(nr_polar_B, nr_polar_CPrime, polarParams->interleaving_pattern, polarParams->K);
#ifdef DEBUG_POLAR_ENCODER
  uint64_t Cprime = 0;

  for (int i = 0; i < polarParams->K; i++) {
    Cprime = Cprime | ((uint64_t)nr_polar_CPrime[i] << i);
    if (nr_polar_CPrime[i] == 1)
      printf("pos %d : %lx\n", i, Cprime);
  }

  printf("polar_Cprime %lx\n", Cprime);
#endif
  // Bit insertion (c' to u)
  uint8_t nr_polar_U[polarParams->N];
  nr_polar_bit_insertion(nr_polar_CPrime,
                         nr_polar_U,
                         polarParams->N,
                         polarParams->K,
                         polarParams->Q_I_N,
                         polarParams->Q_PC_N,
                         polarParams->n_pc);
  uint8_t nr_polar_D[polarParams->N];
  nr_matrix_multiplication_uint8_1D_uint8_2D(nr_polar_U, polarParams->G_N, nr_polar_D, polarParams->N, polarParams->N);

  for (uint i = 0; i < polarParams->N; i++)
    nr_polar_D[i] %= 2;

  uint64_t D[8];
  memset(D, 0, sizeof(D));
#ifdef DEBUG_POLAR_ENCODER

  for (int i = 0; i < polarParams->N; i++)
    D[i / 64] |= ((uint64_t)nr_polar_D[i]) << (i & 63);

  printf("D %llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx\n", D[0], D[1], D[2], D[3], D[4], D[5], D[6], D[7]);
#endif
  // Rate matching
  // Sub-block interleaving (d to y) and Bit selection (y to e)
  uint8_t nr_polar_E[polarParams->encoderLength];
  nr_polar_interleaver(nr_polar_D, nr_polar_E, polarParams->rate_matching_pattern, polarParams->encoderLength);
  /*
   * Return bits.
   */
#ifdef DEBUG_POLAR_ENCODER

  for (int i = 0; i < polarParams->encoderLength; i++)
    printf("f[%d]=%d\n", i, nr_polar_E[i]);

#endif
  nr_byte2bit_uint8_32(nr_polar_E, polarParams->encoderLength, out);

  polarReturn(polarParams);
}

void polar_encoder_dci(uint32_t *in,
                       uint32_t *out,
                       int8_t messageType,
                       uint16_t messageLength,
                       uint8_t aggregation_level,
                       uint16_t n_RNTI)
{
  t_nrPolar_params *polarParams = nr_polar_params(messageType, messageLength, aggregation_level);

#ifdef DEBUG_POLAR_ENCODER_DCI
  printf("[polar_encoder_dci] in: [0]->0x%08x \t [1]->0x%08x \t [2]->0x%08x \t [3]->0x%08x\n", in[0], in[1], in[2], in[3]);
#endif
  /*
   * Bytewise operations
   */
  //(a to a')
  uint8_t nr_polar_A[polarParams->payloadBits];
  nr_bit2byte_uint32_8(in, polarParams->payloadBits, nr_polar_A);
  uint8_t nr_polar_APrime[polarParams->K];
  for (int i = 0; i < polarParams->crcParityBits; i++)
    nr_polar_APrime[i] = 1;
  const int end = polarParams->crcParityBits + polarParams->payloadBits;
  for (int i = polarParams->crcParityBits; i < end; i++)
    nr_polar_APrime[i] = nr_polar_A[i];

#ifdef DEBUG_POLAR_ENCODER_DCI
  printf("[polar_encoder_dci] A: ");
  for (int i = 0; i < polarParams->payloadBits; i++)
    printf("%d-", nr_polar_A[i]);
  printf("\n");

  printf("[polar_encoder_dci] APrime: ");
  for (int i = 0; i < polarParams->K; i++)
    printf("%d-", nr_polar_APrime[i]);
  printf("\n");

  printf("[polar_encoder_dci] GP: ");
  for (int i = 0; i < polarParams->crcParityBits; i++)
    printf("%d-", polarParams->crc_generator_matrix[0][i]);
  printf("\n");
#endif
  // Calculate CRC.
  uint8_t nr_polar_crc[polarParams->crcParityBits];
  nr_matrix_multiplication_uint8_1D_uint8_2D(nr_polar_APrime,
                                             polarParams->crc_generator_matrix,
                                             nr_polar_crc,
                                             polarParams->K,
                                             polarParams->crcParityBits);

  for (uint i = 0; i < polarParams->crcParityBits; i++)
    nr_polar_crc[i] %= 2;

#ifdef DEBUG_POLAR_ENCODER_DCI
  printf("[polar_encoder_dci] CRC: ");
  for (int i = 0; i < polarParams->crcParityBits; i++)
    printf("%d-", nr_polar_crc[i]);
  printf("\n");
#endif
  uint8_t nr_polar_B[polarParams->payloadBits + 8 + 16];
  // Attach CRC to the Transport Block. (a to b)
  memcpy(nr_polar_B, nr_polar_A, polarParams->payloadBits);

  for (uint i = polarParams->payloadBits; i < polarParams->K; i++)
    nr_polar_B[i] = nr_polar_crc[i - polarParams->payloadBits];

  // Scrambling (b to c)
  for (int i = 0; i < 16; i++)
    nr_polar_B[polarParams->payloadBits + 8 + i] = (nr_polar_B[polarParams->payloadBits + 8 + i] + ((n_RNTI >> (15 - i)) & 1)) % 2;

#ifdef DEBUG_POLAR_ENCODER_DCI
  printf("[polar_encoder_dci] B: ");
  for (int i = 0; i < polarParams->K; i++)
    printf("%d-", nr_polar_B[i]);
  printf("\n");
#endif
  // Interleaving (c to c')
  uint8_t nr_polar_CPrime[polarParams->K];
  nr_polar_interleaver(nr_polar_B, nr_polar_CPrime, polarParams->interleaving_pattern, polarParams->K);
  // Bit insertion (c' to u)
  uint8_t nr_polar_U[polarParams->N];
  nr_polar_bit_insertion(nr_polar_CPrime,
                         nr_polar_U,
                         polarParams->N,
                         polarParams->K,
                         polarParams->Q_I_N,
                         polarParams->Q_PC_N,
                         polarParams->n_pc);
  // Encoding (u to d)
  uint8_t nr_polar_D[polarParams->N];
  nr_matrix_multiplication_uint8_1D_uint8_2D(nr_polar_U, polarParams->G_N, nr_polar_D, polarParams->N, polarParams->N);
  for (uint i = 0; i < polarParams->N; i++)
    nr_polar_D[i] %= 2;

  // Rate matching
  // Sub-block interleaving (d to y) and Bit selection (y to e)
  uint8_t nr_polar_E[polarParams->encoderLength];
  nr_polar_interleaver(nr_polar_D, nr_polar_E, polarParams->rate_matching_pattern, polarParams->encoderLength);
  /*
   * Return bits.
   */
  nr_byte2bit_uint8_32(nr_polar_E, polarParams->encoderLength, out);
#ifdef DEBUG_POLAR_ENCODER_DCI
  printf("[polar_encoder_dci] E: ");
  for (int i = 0; i < polarParams->encoderLength; i++)
    printf("%d-", nr_polar_E[i]);

  uint8_t outputInd = ceil(polarParams->encoderLength / 32.0);
  printf("\n[polar_encoder_dci] out: ");
  for (int i = 0; i < outputInd; i++)
    printf("[%d]->0x%08x\t", i, out[i]);
#endif
  polarReturn(polarParams);
}

////////////////////////////////////////////////
////////////////////decoder/////////////////////
////////////////////////////////////////////////

static inline void updateCrcChecksum2(int xlen,
                                      int ylen,
                                      uint8_t crcChecksum[xlen][ylen],
                                      int gxlen,
                                      int gylen,
                                      uint8_t crcGen[gxlen][gylen],
                                      uint8_t listSize,
                                      uint32_t i2,
                                      uint8_t len)
{
  for (uint i = 0; i < listSize; i++) {
    for (uint j = 0; j < len; j++) {
      crcChecksum[j][i + listSize] = (crcChecksum[j][i] + crcGen[i2][j]) % 2;
    }
  }
}

int8_t polar_decoder_legacy(double *input,
                     uint32_t *out,
                     uint8_t listSize,
                     int8_t messageType,
                     uint16_t messageLength,
                     uint8_t aggregation_level)
{
  t_nrPolar_params *polarParams = nr_polar_params(messageType, messageLength, aggregation_level);
  // Assumes no a priori knowledge.
  uint8_t bit[polarParams->N][polarParams->n + 1][2 * listSize];
  memset(bit, 0, sizeof bit);
  uint8_t bitUpdated[polarParams->N][polarParams->n + 1]; // 0=False, 1=True
  memset(bitUpdated, 0, sizeof bitUpdated);
  uint8_t llrUpdated[polarParams->N][polarParams->n + 1]; // 0=False, 1=True
  memset(llrUpdated, 0, sizeof llrUpdated);
  double llr[polarParams->N][polarParams->n + 1][2 * listSize];
  uint8_t crcChecksum[polarParams->crcParityBits][2 * listSize];
  memset(crcChecksum, 0, sizeof crcChecksum);
  double pathMetric[2 * listSize];
  uint8_t crcState[2 * listSize]; // 0=False, 1=True

  for (int i = 0; i < (2 * listSize); i++) {
    pathMetric[i] = 0;
    crcState[i] = 1;
  }

  for (int i = 0; i < polarParams->N; i++) {
    llrUpdated[i][polarParams->n] = 1;
    bitUpdated[i][0] = (polarParams->information_bit_pattern[i] + 1) % 2;
  }

  uint8_t extended_crc_generator_matrix[polarParams->K][polarParams->crcParityBits]; // G_P3
  uint8_t tempECGM[polarParams->K][polarParams->crcParityBits]; // G_P2

  for (int i = 0; i < polarParams->payloadBits; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      tempECGM[i][j] = polarParams->crc_generator_matrix[i][j];
    }
  }

  for (int i = polarParams->payloadBits; i < polarParams->K; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      tempECGM[i][j] = (i - polarParams->payloadBits) == j;
    }
  }

  for (int i = 0; i < polarParams->K; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      extended_crc_generator_matrix[i][j] = tempECGM[polarParams->interleaving_pattern[i]][j];
    }
  }

  // The index of the last 1-valued bit that appears in each column.
  AssertFatal(polarParams->crcParityBits > 0, "UB for VLA. crcParityBits negative\n");
  uint16_t last1ind[polarParams->crcParityBits];

  for (int j = 0; j < polarParams->crcParityBits; j++) {
    for (int i = 0; i < polarParams->K; i++) {
      if (extended_crc_generator_matrix[i][j] == 1)
        last1ind[j] = i;
    }
  }

  double d_tilde[polarParams->N];
  nr_polar_rate_matching(input,
                         d_tilde,
                         polarParams->rate_matching_pattern,
                         polarParams->K,
                         polarParams->N,
                         polarParams->encoderLength);

  for (int j = 0; j < polarParams->N; j++)
    llr[j][polarParams->n][0] = d_tilde[j];

  /*
   * SCL polar decoder.
   */
  uint32_t nonFrozenBit = 0;
  uint currentListSize = 1;
  uint decoderIterationCheck = 0;
  int checkCrcBits = -1;
  uint8_t listIndex[2 * listSize], copyIndex;

  for (uint currentBit = 0; currentBit < polarParams->N; currentBit++) {
    updateLLR(currentListSize, currentBit, 0, polarParams->N, polarParams->n + 1, 2 * listSize, llr, llrUpdated, bit, bitUpdated);

    if (polarParams->information_bit_pattern[currentBit] == 0) { // Frozen bit.
      updatePathMetric(pathMetric, currentListSize, 0, currentBit, polarParams->N, polarParams->n + 1, 2 * listSize, llr);
    } else { // Information or CRC bit.
      updatePathMetric2(pathMetric, currentListSize, currentBit, polarParams->N, polarParams->n + 1, 2 * listSize, llr);

      for (int i = 0; i < currentListSize; i++) {
        for (int j = 0; j < polarParams->N; j++) {
          for (int k = 0; k < (polarParams->n + 1); k++) {
            bit[j][k][i + currentListSize] = bit[j][k][i];
            llr[j][k][i + currentListSize] = llr[j][k][i];
          }
        }
      }

      for (int i = 0; i < currentListSize; i++) {
        bit[currentBit][0][i] = 0;
        crcState[i + currentListSize] = crcState[i];
      }

      for (int i = currentListSize; i < 2 * currentListSize; i++)
        bit[currentBit][0][i] = 1;

      bitUpdated[currentBit][0] = 1;
      updateCrcChecksum2(polarParams->crcParityBits,
                         2 * listSize,
                         crcChecksum,
                         polarParams->K,
                         polarParams->crcParityBits,
                         extended_crc_generator_matrix,
                         currentListSize,
                         nonFrozenBit,
                         polarParams->crcParityBits);
      currentListSize *= 2;

      // Keep only the best "listSize" number of entries.
      if (currentListSize > listSize) {
        for (uint i = 0; i < 2 * listSize; i++)
          listIndex[i] = i;

        nr_sort_asc_double_1D_array_ind(pathMetric, listIndex, currentListSize);
        // sort listIndex[listSize, ..., 2*listSize-1] in descending order.

        for (uint i = 0; i < listSize; i++) {
          int swaps = 0;

          for (uint j = listSize; j < (2 * listSize - i) - 1; j++) {
            if (listIndex[j + 1] > listIndex[j]) {
              int tempInd = listIndex[j];
              listIndex[j] = listIndex[j + 1];
              listIndex[j + 1] = tempInd;
              swaps++;
            }
          }

          if (swaps == 0)
            break;
        }

        // First, backup the best "listSize" number of entries.
        for (int k = (listSize - 1); k > 0; k--) {
          for (int i = 0; i < polarParams->N; i++) {
            for (int j = 0; j < (polarParams->n + 1); j++) {
              bit[i][j][listIndex[(2 * listSize - 1) - k]] = bit[i][j][listIndex[k]];
              llr[i][j][listIndex[(2 * listSize - 1) - k]] = llr[i][j][listIndex[k]];
            }
          }
        }

        for (int k = (listSize - 1); k > 0; k--) {
          for (int i = 0; i < polarParams->crcParityBits; i++) {
            crcChecksum[i][listIndex[(2 * listSize - 1) - k]] = crcChecksum[i][listIndex[k]];
          }
        }

        for (int k = (listSize - 1); k > 0; k--)
          crcState[listIndex[(2 * listSize - 1) - k]] = crcState[listIndex[k]];

        // Copy the best "listSize" number of entries to the first indices.
        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          for (int i = 0; i < polarParams->N; i++) {
            for (int j = 0; j < (polarParams->n + 1); j++) {
              bit[i][j][k] = bit[i][j][copyIndex];
              llr[i][j][k] = llr[i][j][copyIndex];
            }
          }
        }

        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          for (int i = 0; i < polarParams->crcParityBits; i++) {
            crcChecksum[i][k] = crcChecksum[i][copyIndex];
          }
        }

        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          crcState[k] = crcState[copyIndex];
        }

        currentListSize = listSize;
      }

      for (int i = 0; i < polarParams->crcParityBits; i++) {
        if (last1ind[i] == nonFrozenBit) {
          checkCrcBits = i;
          break;
        }
      }

      if (checkCrcBits > (-1)) {
        for (uint i = 0; i < currentListSize; i++) {
          if (crcChecksum[checkCrcBits][i] == 1) {
            crcState[i] = 0; // 0=False, 1=True
          }
        }
      }

      for (uint i = 0; i < currentListSize; i++)
        decoderIterationCheck += crcState[i];

      if (decoderIterationCheck == 0) {
        // perror("[SCL polar decoder] All list entries have failed the CRC checks.");
        polarReturn(polarParams);
        return -1;
      }

      nonFrozenBit++;
      decoderIterationCheck = 0;
      checkCrcBits = -1;
    }
  }

  for (uint i = 0; i < 2 * listSize; i++)
    listIndex[i] = i;

  nr_sort_asc_double_1D_array_ind(pathMetric, listIndex, currentListSize);
  uint8_t nr_polar_A[polarParams->payloadBits];
  for (uint i = 0; i < fmin(listSize, (pow(2, polarParams->consts.crcCorrectionBits))); i++) {
    if (crcState[listIndex[i]] == 1) {
      uint8_t nr_polar_U[polarParams->N];
      for (int j = 0; j < polarParams->N; j++)
        nr_polar_U[j] = bit[j][0][listIndex[i]];

      // Extract the information bits (û to ĉ)
      uint8_t nr_polar_CPrime[polarParams->N];
      nr_polar_info_bit_extraction(nr_polar_U, nr_polar_CPrime, polarParams->information_bit_pattern, polarParams->N);
      // Deinterleaving (ĉ to b)
      uint8_t nr_polar_B[polarParams->K];
      nr_polar_deinterleaver(nr_polar_CPrime, nr_polar_B, polarParams->interleaving_pattern, polarParams->K);

      // Remove the CRC (â)
      memcpy(nr_polar_A, nr_polar_B, polarParams->payloadBits);

      break;
    }
  }

  /*
   * Return bits.
   */
  nr_byte2bit_uint8_32(nr_polar_A, polarParams->payloadBits, out);

  polarReturn(polarParams);
  return 0;
}

int8_t polar_decoder_dci(double *input,
                         uint32_t *out,
                         uint8_t listSize,
                         uint16_t n_RNTI,
                         int8_t messageType,
                         uint16_t messageLength,
                         uint8_t aggregation_level)
{
  t_nrPolar_params *polarParams = nr_polar_params(messageType, messageLength, aggregation_level);

  uint8_t bit[polarParams->N][polarParams->n + 1][2 * listSize];
  memset(bit, 0, sizeof bit);
  uint8_t bitUpdated[polarParams->N][polarParams->n + 1]; // 0=False, 1=True
  memset(bitUpdated, 0, sizeof bitUpdated);
  uint8_t llrUpdated[polarParams->N][polarParams->n + 1]; // 0=False, 1=True
  memset(llrUpdated, 0, sizeof llrUpdated);
  double llr[polarParams->N][polarParams->n + 1][2 * listSize];
  uint8_t crcChecksum[polarParams->crcParityBits][2 * listSize];
  memset(crcChecksum, 0, sizeof crcChecksum);
  double pathMetric[2 * listSize];
  uint8_t crcState[2 * listSize]; // 0=False, 1=True
  uint8_t extended_crc_scrambling_pattern[polarParams->crcParityBits];

  for (int i = 0; i < (2 * listSize); i++) {
    pathMetric[i] = 0;
    crcState[i] = 1;
  }

  for (int i = 0; i < polarParams->N; i++) {
    llrUpdated[i][polarParams->n] = 1;
    bitUpdated[i][0] = (polarParams->information_bit_pattern[i] + 1) % 2;
  }

  uint8_t extended_crc_generator_matrix[polarParams->K][polarParams->crcParityBits]; // G_P3: K-by-P
  uint8_t tempECGM[polarParams->K][polarParams->crcParityBits]; // G_P2: K-by-P

  for (int i = 0; i < polarParams->payloadBits; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      tempECGM[i][j] = polarParams->crc_generator_matrix[i + polarParams->crcParityBits][j];
    }
  }

  for (int i = polarParams->payloadBits; i < polarParams->K; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      tempECGM[i][j] = (i - polarParams->payloadBits) == j;
    }
  }

  for (int i = 0; i < polarParams->K; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++) {
      extended_crc_generator_matrix[i][j] = tempECGM[polarParams->interleaving_pattern[i]][j];
    }
  }

  // The index of the last 1-valued bit that appears in each column.
  uint16_t last1ind[polarParams->crcParityBits];

  for (int j = 0; j < polarParams->crcParityBits; j++) {
    for (int i = 0; i < polarParams->K; i++) {
      if (extended_crc_generator_matrix[i][j] == 1)
        last1ind[j] = i;
    }
  }

  for (int i = 0; i < 8; i++)
    extended_crc_scrambling_pattern[i] = 0;

  for (int i = 8; i < polarParams->crcParityBits; i++) {
    extended_crc_scrambling_pattern[i] = (n_RNTI >> (23 - i)) & 1;
  }

  double d_tilde[polarParams->N];
  nr_polar_rate_matching(input,
                         d_tilde,
                         polarParams->rate_matching_pattern,
                         polarParams->K,
                         polarParams->N,
                         polarParams->encoderLength);

  for (int j = 0; j < polarParams->N; j++)
    llr[j][polarParams->n][0] = d_tilde[j];

  /*
   * SCL polar decoder.
   */

  for (int i = 0; i < polarParams->crcParityBits; i++) {
    for (int j = 0; j < polarParams->crcParityBits; j++)
      crcChecksum[i][0] = crcChecksum[i][0] + polarParams->crc_generator_matrix[j][i];

    crcChecksum[i][0] = (crcChecksum[i][0] % 2);
  }

  uint32_t nonFrozenBit = 0;
  uint currentListSize = 1;
  uint decoderIterationCheck = 0;
  int checkCrcBits = -1;
  uint8_t listIndex[2 * listSize], copyIndex;

  for (uint currentBit = 0; currentBit < polarParams->N; currentBit++) {
    updateLLR(currentListSize, currentBit, 0, polarParams->N, polarParams->n + 1, 2 * listSize, llr, llrUpdated, bit, bitUpdated);

    if (polarParams->information_bit_pattern[currentBit] == 0) { // Frozen bit.
      updatePathMetric(pathMetric, currentListSize, 0, currentBit, polarParams->N, polarParams->n + 1, 2 * listSize, llr);
    } else { // Information or CRC bit.
      updatePathMetric2(pathMetric, currentListSize, currentBit, polarParams->N, polarParams->n + 1, 2 * listSize, llr);

      for (int i = 0; i < currentListSize; i++) {
        for (int j = 0; j < polarParams->N; j++) {
          for (int k = 0; k < (polarParams->n + 1); k++) {
            bit[j][k][i + currentListSize] = bit[j][k][i];
            llr[j][k][i + currentListSize] = llr[j][k][i];
          }
        }
      }

      for (int i = 0; i < currentListSize; i++) {
        bit[currentBit][0][i] = 0;
        crcState[i + currentListSize] = crcState[i];
      }

      for (int i = currentListSize; i < 2 * currentListSize; i++)
        bit[currentBit][0][i] = 1;

      bitUpdated[currentBit][0] = 1;
      updateCrcChecksum2(polarParams->crcParityBits,
                         2 * listSize,
                         crcChecksum,
                         polarParams->K,
                         polarParams->crcParityBits,
                         extended_crc_generator_matrix,
                         currentListSize,
                         nonFrozenBit,
                         polarParams->crcParityBits);
      currentListSize *= 2;

      // Keep only the best "listSize" number of entries.
      if (currentListSize > listSize) {
        for (uint i = 0; i < 2 * listSize; i++)
          listIndex[i] = i;

        nr_sort_asc_double_1D_array_ind(pathMetric, listIndex, currentListSize);
        // sort listIndex[listSize, ..., 2*listSize-1] in descending order.

        for (uint i = 0; i < listSize; i++) {
          int swaps = 0;

          for (uint j = listSize; j < (2 * listSize - i) - 1; j++) {
            if (listIndex[j + 1] > listIndex[j]) {
              int tempInd = listIndex[j];
              listIndex[j] = listIndex[j + 1];
              listIndex[j + 1] = tempInd;
              swaps++;
            }
          }

          if (swaps == 0)
            break;
        }

        // First, backup the best "listSize" number of entries.
        for (int k = (listSize - 1); k > 0; k--) {
          for (int i = 0; i < polarParams->N; i++) {
            for (int j = 0; j < (polarParams->n + 1); j++) {
              bit[i][j][listIndex[(2 * listSize - 1) - k]] = bit[i][j][listIndex[k]];
              llr[i][j][listIndex[(2 * listSize - 1) - k]] = llr[i][j][listIndex[k]];
            }
          }
        }

        for (int k = (listSize - 1); k > 0; k--) {
          for (int i = 0; i < polarParams->crcParityBits; i++) {
            crcChecksum[i][listIndex[(2 * listSize - 1) - k]] = crcChecksum[i][listIndex[k]];
          }
        }

        for (int k = (listSize - 1); k > 0; k--)
          crcState[listIndex[(2 * listSize - 1) - k]] = crcState[listIndex[k]];

        // Copy the best "listSize" number of entries to the first indices.
        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          for (int i = 0; i < polarParams->N; i++) {
            for (int j = 0; j < (polarParams->n + 1); j++) {
              bit[i][j][k] = bit[i][j][copyIndex];
              llr[i][j][k] = llr[i][j][copyIndex];
            }
          }
        }

        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          for (int i = 0; i < polarParams->crcParityBits; i++) {
            crcChecksum[i][k] = crcChecksum[i][copyIndex];
          }
        }

        for (int k = 0; k < listSize; k++) {
          if (k > listIndex[k]) {
            copyIndex = listIndex[(2 * listSize - 1) - k];
          } else { // Use the backup.
            copyIndex = listIndex[k];
          }

          crcState[k] = crcState[copyIndex];
        }

        currentListSize = listSize;
      }

      for (int i = 0; i < polarParams->crcParityBits; i++) {
        if (last1ind[i] == nonFrozenBit) {
          checkCrcBits = i;
          break;
        }
      }

      if (checkCrcBits > (-1)) {
        for (uint i = 0; i < currentListSize; i++) {
          if (crcChecksum[checkCrcBits][i] != extended_crc_scrambling_pattern[checkCrcBits]) {
            crcState[i] = 0; // 0=False, 1=True
          }
        }
      }

      for (uint i = 0; i < currentListSize; i++)
        decoderIterationCheck += crcState[i];

      if (decoderIterationCheck == 0) {
        // perror("[SCL polar decoder] All list entries have failed the CRC checks.");
        polarReturn(polarParams);
        return -1;
      }

      nonFrozenBit++;
      decoderIterationCheck = 0;
      checkCrcBits = -1;
    }
  }

  for (uint i = 0; i < 2 * listSize; i++)
    listIndex[i] = i;

  nr_sort_asc_double_1D_array_ind(pathMetric, listIndex, currentListSize);
  uint8_t nr_polar_A[polarParams->payloadBits];

  for (uint i = 0; i < fmin(listSize, (pow(2, polarParams->consts.crcCorrectionBits))); i++) {
    if (crcState[listIndex[i]] == 1) {
      uint8_t nr_polar_U[polarParams->N];
      for (int j = 0; j < polarParams->N; j++)
        nr_polar_U[j] = bit[j][0][listIndex[i]];

      // Extract the information bits (û to ĉ)
      uint8_t nr_polar_CPrime[polarParams->N];
      nr_polar_info_bit_extraction(nr_polar_U, nr_polar_CPrime, polarParams->information_bit_pattern, polarParams->N);
      // Deinterleaving (ĉ to b)
      uint8_t nr_polar_B[polarParams->K];
      nr_polar_deinterleaver(nr_polar_CPrime, nr_polar_B, polarParams->interleaving_pattern, polarParams->K);

      // Remove the CRC (â)
      memcpy(nr_polar_A, nr_polar_B, polarParams->payloadBits);
      break;
    }
  }

  /*
   * Return bits.
   */
  nr_byte2bit_uint8_32(nr_polar_A, polarParams->payloadBits, out);

  polarReturn(polarParams);
  return 0;
}


// =============================================
//                 end references
// =============================================


configmodule_interface_t *uniqCfg = NULL;
int main(int argc, char *argv[])
{
  // Default simulation values (Aim for iterations = 1000000.)
  int ret = 1;
  int decoder_int16 = 0;
  int itr, iterations = 1000, arguments;
  polar_type_t polarMessageType = NR_POLAR_PBCH_MESSAGE_TYPE;
  double SNRstart = -20.0, SNRstop = 0.0, SNRinc = 0.5; // dB
  double SNR, SNR_lin;
  int16_t nBitError = 0; // -1 = Decoding failed (All list entries have failed the CRC checks).
  uint32_t decoderState = 0, blockErrorState = 0; // 0 = Success, -1 = Decoding failed, 1 = Block Error.
  uint16_t testLength = NR_POLAR_PBCH_PAYLOAD_BITS, coderLength = NR_POLAR_PBCH_E;
  uint16_t blockErrorCumulative = 0, bitErrorCumulative = 0;
  uint8_t aggregation_level = 8, decoderListSize = 8, logFlag = 0;
  uint16_t rnti = 0;

  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == 0) {
    exit_fun("[POLARTEST] Error, configuration module init failed\n");
  }
  logInit();
  // load polar interface
  int ret_po = load_polar_interface(NULL, &polar_interface);
  AssertFatal(ret_po == 0, "Failed to load Polar interface\n");

  while ((arguments = getopt(argc, argv, "--:O:s:d:f:m:i:l:a:p:hqgFL:k:")) != -1) {
    /* ignore long options starting with '--', option '-O' and their arguments that are handled by configmodule */
    /* with this opstring getopt returns 1 for non-option arguments, refer to 'man 3 getopt' */
    if (arguments == 1 || arguments == '-' || arguments == 'O')
      continue;

    printf("handling optarg %c\n", arguments);
    switch (arguments) {
      case 's':
        SNRstart = atof(optarg);
        SNRstop = SNRstart + 2;
        break;

      case 'd':
        SNRinc = atof(optarg);
        break;

      case 'f':
        SNRstop = atof(optarg);
        break;

      case 'm':
        polarMessageType = atoi(optarg);
        if (polarMessageType != 0 && polarMessageType != 1 && polarMessageType != 2)
          printf("Illegal polar message type %d (should be 0,1 or 2)\n", polarMessageType);
        break;

      case 'i':
        iterations = atoi(optarg);
        break;

      case 'l':
        decoderListSize = (uint8_t)atoi(optarg);
        break;

      case 'q':
        decoder_int16 = 1;
        break;

      case 'g':
        iterations = 1;
        SNRstart = -6.0;
        SNRstop = -6.0;
        decoder_int16 = 1;
        break;

      case 'F':
        logFlag = 1;
        break;

      case 'L':
        aggregation_level = atoi(optarg);
        if (aggregation_level != 1 && aggregation_level != 2 && aggregation_level != 4 && aggregation_level != 8
            && aggregation_level != 16) {
          printf("Illegal aggregation level %d \n", aggregation_level);
          exit(-1);
        }
        break;

      case 'k':
        testLength = atoi(optarg);
        if (testLength < 12 || testLength > 127) {
          printf("Illegal packet bitlength %d \n", testLength);
          exit(-1);
        }
        break;

      case 'h':
        printf(
            "./polartest\nOptions\n-h Print this help\n-s SNRstart (dB)\n-d SNRinc (dB)\n-f SNRstop (dB)\n-m [0=PBCH|1=DCI|2=UCI]\n"
            "-i Number of iterations\n-l decoderListSize\n-q Flag for optimized coders usage\n-F Flag for test results logging\n"
            "-L aggregation level (for DCI)\n-k packet_length (bits) for DCI/UCI\n");
        exit(-1);
        break;

      default:
        perror("[polartest.c] Problem at argument parsing with getopt");
        exit(-1);
        break;
    }
  }
  // Initiate timing. (Results depend on CPU Frequency. Therefore, might change due to performance variances during simulation.)
  time_stats_t timeEncoder, timeDecoder;
  cpu_meas_enabled = 1;
  reset_meas(&timeEncoder);
  reset_meas(&timeDecoder);
  randominit();
  crcTableInit();

  if (polarMessageType == NR_POLAR_PBCH_MESSAGE_TYPE) {
    aggregation_level = NR_POLAR_PBCH_AGGREGATION_LEVEL;
  } else if (polarMessageType == NR_POLAR_DCI_MESSAGE_TYPE) {
    coderLength = 108 * aggregation_level;
  } else if (polarMessageType == NR_POLAR_UCI_MESSAGE_TYPE) {
    // pucch2 parameters, 1 symbol, aggregation_level = NPRB
    AssertFatal(aggregation_level > 2, "For UCI formats, aggregation (N_RB) should be > 2\n");
    coderLength = 16 * aggregation_level;
  }

  // Logging
  time_t currentTime;
  char fileName[512], currentTimeInfo[25];
  char folderName[] = ".";
  FILE *logFile = NULL;

  if (logFlag) {
    time(&currentTime);
#ifdef DEBUG_POLAR_TIMING
    sprintf(fileName, "%s/TIMING_ListSize_%d_Payload_%d_Itr_%d", folderName, decoderListSize, testLength, iterations);
#else
    sprintf(fileName, "%s/_ListSize_%d_Payload_%d_Itr_%d", folderName, decoderListSize, testLength, iterations);
#endif
    strftime(currentTimeInfo, 25, "_%Y-%m-%d-%H-%M-%S.csv", localtime(&currentTime));
    strcat(fileName, currentTimeInfo);
    // Create "~/Desktop/polartestResults" folder if it doesn't already exist.
    /*struct stat folder = {0};
      if (stat(folderName, &folder) == -1) mkdir(folderName, S_IRWXU | S_IRWXG | S_IRWXO);*/
    logFile = fopen(fileName, "w");

    if (logFile == NULL) {
      fprintf(stderr, "[polartest.c] Problem creating file %s with fopen\n", fileName);
      exit(-1);
    }

#ifdef DEBUG_POLAR_TIMING
    fprintf(logFile,
            ",timeEncoderCRCByte[us],timeEncoderCRCBit[us],timeEncoderInterleaver[us],timeEncoderBitInsertion[us],timeEncoder1[us],"
            "timeEncoder2[us],timeEncoderRateMatching[us],timeEncoderByte2Bit[us]\n");
#else
    fprintf(logFile, ",SNR,nBitError,blockErrorState,t_encoder[us],t_decoder[us]\n");
#endif
  }

  const uint8_t testArrayLength = ceil(testLength / 32.0);
  const uint8_t coderArrayLength = ceil(coderLength / 32.0);
  // in the polar code, often uint64_t arrays are used, but we work with
  // uint32_t arrays below, so realArrayLength is the length that always
  // satisfies uint64_t array length
  const uint8_t realArrayLength = ((testArrayLength + 1) / 2) * 2;
  printf("testArrayLength %d realArrayLength %d\n", testArrayLength, realArrayLength);
  uint32_t testInput[realArrayLength]; // generate randomly
  uint32_t encoderOutput[coderArrayLength];
  uint32_t estimatedOutput[realArrayLength]; // decoder output
  memset(testInput, 0, sizeof(uint32_t) * realArrayLength); // does not reset all
  memset(encoderOutput, 0, sizeof(uint32_t) * coderArrayLength);
  memset(estimatedOutput, 0, sizeof(uint32_t) * realArrayLength);
  uint8_t encoderOutputByte[coderLength];
  double modulatedInput[coderLength]; // channel input
  double channelOutput[coderLength]; // add noise
  int16_t channelOutput_int16[coderLength];

  t_nrPolar_params *currentPtr = nr_polar_params(polarMessageType, testLength, aggregation_level);

#ifdef DEBUG_DCI_POLAR_PARAMS
  uint32_t dci_pdu[4];
  memset(dci_pdu, 0, sizeof(uint32_t) * 4);
  dci_pdu[0] = 0x01189400;
  printf("dci_pdu: [0]->0x%08x \t [1]->0x%08x \t [2]->0x%08x \t [3]->0x%08x\n", dci_pdu[0], dci_pdu[1], dci_pdu[2], dci_pdu[3]);
  uint16_t size = 41;
  rnti = 3;
  aggregation_level = 8;
  uint32_t encoder_output[54];
  memset(encoder_output, 0, sizeof(uint32_t) * 54);
  t_nrPolar_params *currentPtrDCI = nr_polar_params(1, size, aggregation_level);

  polar_encoder_dci(dci_pdu, encoder_output, currentPtrDCI, rnti);
  for (int i = 0; i < 54; i++)
    printf("encoder_output: [%2d]->0x%08x \n", i, encoder_output[i]);

  uint8_t *encoder_outputByte = malloc(sizeof(uint8_t) * currentPtrDCI->encoderLength);
  double *modulated_input = malloc(sizeof(double) * currentPtrDCI->encoderLength);
  double *channel_output = malloc(sizeof(double) * currentPtrDCI->encoderLength);
  uint32_t dci_est[4];
  memset(dci_est, 0, sizeof(dci_est));
  nr_bit2byte_uint32_8(encoder_output, currentPtrDCI->encoderLength, encoder_outputByte);
  printf("[polartest] encoder_outputByte: ");
  for (int i = 0; i < currentPtrDCI->encoderLength; i++)
    printf("%d-", encoder_outputByte[i]);
  printf("\n");

  SNR_lin = pow(10, 0 / 10); // SNR = 0 dB
  for (int i = 0; i < currentPtrDCI->encoderLength; i++) {
    if (encoder_outputByte[i] == 0)
      modulated_input[i] = 1 / sqrt(2);
    else
      modulated_input[i] = (-1) / sqrt(2);
    channel_output[i] = modulated_input[i] + (gaussdouble(0.0, 1.0) * (1 / sqrt(2 * SNR_lin)));
  }
  decoderState = polar_decoder_dci(channel_output, dci_est, NR_POLAR_DECODER_LISTSIZE, rnti, 1, size, aggregation_level);
  printf("dci_est: [0]->0x%08x \t [1]->0x%08x \t [2]->0x%08x \t [3]->0x%08x\n", dci_est[0], dci_est[1], dci_est[2], dci_est[3]);
  free(encoder_outputByte);
  free(channel_output);
  free(modulated_input);
  if (logFlag)
    fclose(logFile);
  return 0;
#endif

  for (SNR = SNRstart; SNR <= SNRstop; SNR += SNRinc) {
    printf("SNR %f\n", SNR);
    SNR_lin = pow(10, SNR / 10);

    for (itr = 1; itr <= iterations; itr++) {
      // Generate random values for all the bits of "testInput", not just as much as "currentPtr->payloadBits".
      for (int i = 0; i < testArrayLength; i++) {
        for (int j = 0; j < (sizeof(testInput[0]) * 8) - 1; j++) {
          testInput[i] |= (((uint32_t)(rand() % 2)) & 1);
          testInput[i] <<= 1;
        }
        testInput[i] |= (((uint32_t)(rand() % 2)) & 1);
      }

#ifdef DEBUG_POLARTEST
      // testInput[0] = 0x360f8a5c;
      printf("testInput: [0]->0x%08x\n", testInput[0]);
#endif
      int len_mod64 = currentPtr->payloadBits & 63;
      ((uint64_t *)testInput)[currentPtr->payloadBits / 64] &= ((((uint64_t)1) << len_mod64) - 1);

      start_meas(&timeEncoder);
      if (decoder_int16 == 1) {
        polar_interface.polar_encoder((uint64_t *)testInput, encoderOutput, polarMessageType, testLength, aggregation_level, 0);
      } else {
        if (polarMessageType == NR_POLAR_PBCH_MESSAGE_TYPE)
          polar_encoder_legacy(testInput, encoderOutput, polarMessageType, testLength, aggregation_level);
        else if (polarMessageType == NR_POLAR_DCI_MESSAGE_TYPE)
          polar_encoder_dci(testInput, encoderOutput, polarMessageType, testLength, aggregation_level, rnti);
      }
      stop_meas(&timeEncoder);

#ifdef DEBUG_POLARTEST
      printf("encoderOutput: [0]->0x%08x\n", encoderOutput[0]);
      // for (int i=1;i<coderArrayLength;i++) printf("encoderOutput: [i]->0x%08x\n", i, encoderOutput[1]);
#endif

      // Bit-to-byte:
      nr_bit2byte_uint32_8(encoderOutput, coderLength, encoderOutputByte);

      // BPSK modulation
      for (int i = 0; i < coderLength; i++) {
        if (encoderOutputByte[i] == 0)
          modulatedInput[i] = 1 / sqrt(2);
        else
          modulatedInput[i] = (-1) / sqrt(2);

        channelOutput[i] = modulatedInput[i] + (gaussdouble(0.0, 1.0) * (1 / sqrt(2 * SNR_lin)));

        if (decoder_int16 == 1) {
          if (channelOutput[i] > 15)
            channelOutput_int16[i] = 127;
          else if (channelOutput[i] < -16)
            channelOutput_int16[i] = -128;
          else
            channelOutput_int16[i] = (int16_t)(8 * channelOutput[i]);
        }
      }

      start_meas(&timeDecoder);

      if (decoder_int16 == 1) {
        decoderState = polar_interface.polar_decoder(channelOutput_int16,
                                           (uint64_t *)estimatedOutput,
                                           polarMessageType,
                                           testLength,
                                           aggregation_level);
      } else {
        if (polarMessageType == NR_POLAR_PBCH_MESSAGE_TYPE) {
          decoderState =
              polar_decoder_legacy(channelOutput, estimatedOutput, decoderListSize, polarMessageType, testLength, aggregation_level);
        } else if (polarMessageType == NR_POLAR_DCI_MESSAGE_TYPE) {
          decoderState = polar_decoder_dci(channelOutput,
                                           estimatedOutput,
                                           decoderListSize,
                                           rnti,
                                           polarMessageType,
                                           testLength,
                                           aggregation_level);
        }
      }
      stop_meas(&timeDecoder);

#ifdef DEBUG_POLARTEST
      printf("estimatedOutput: [0]->0x%08x\n", estimatedOutput[0]);
#endif

      // calculate errors
      if (decoderState != 0) {
        blockErrorState = -1;
        nBitError = -1;
      } else {
        for (int i = 0; i < (testArrayLength - 1); i++) {
          for (int j = 0; j < 32; j++) {
            if (((estimatedOutput[i] >> j) & 1) != ((testInput[i] >> j) & 1))
              nBitError++;
          }
        }
        for (int j = 0; j < testLength - ((testArrayLength - 1) * 32); j++)
          if (((estimatedOutput[(testArrayLength - 1)] >> j) & 1) != ((testInput[(testArrayLength - 1)] >> j) & 1))
            nBitError++;
      }
      if (nBitError > 0)
        blockErrorState = 1;
#ifdef DEBUG_POLARTEST
      for (int i = 0; i < testArrayLength; i++)
        printf("[polartest/decoderState=%u] testInput[%d]=0x%08x, estimatedOutput[%d]=0x%08x\n",
               decoderState,
               i,
               testInput[i],
               i,
               estimatedOutput[i]);
#endif

      // Iteration times are in microseconds.
      if (logFlag)
        fprintf(logFile,
                ",%f,%d,%u,%f,%f\n",
                SNR,
                nBitError,
                blockErrorState,
                (timeEncoder.diff / (get_cpu_freq_GHz() * 1000.0)),
                (timeDecoder.diff / (get_cpu_freq_GHz() * 1000.0)));

      if (nBitError < 0) {
        blockErrorCumulative++;
        bitErrorCumulative += testLength;
      } else {
        blockErrorCumulative += blockErrorState;
        bitErrorCumulative += nBitError;
      }

      decoderState = 0;
      nBitError = 0;
      blockErrorState = 0;
      memset(testInput, 0, sizeof(uint32_t) * realArrayLength);
      memset(encoderOutput, 0, sizeof(uint32_t) * coderArrayLength);
      memset(estimatedOutput, 0, sizeof(uint32_t) * realArrayLength);
    }

    // Calculate error statistics for the SNR.
    printf("[ListSize=%d] SNR=%+8.3f, BLER=%9.6f, BER=%12.9f, t_Encoder=%9.3fus, t_Decoder=%9.3fus\n",
           decoderListSize,
           SNR,
           ((double)blockErrorCumulative / iterations),
           ((double)bitErrorCumulative / (iterations * testLength)),
           (double)timeEncoder.diff / timeEncoder.trials / (get_cpu_freq_GHz() * 1000.0),
           (double)timeDecoder.diff / timeDecoder.trials / (get_cpu_freq_GHz() * 1000.0));

    if (blockErrorCumulative == 0 && bitErrorCumulative == 0) {
      ret = 0;
      break;
    }

    blockErrorCumulative = 0;
    bitErrorCumulative = 0;
  }

  print_meas(&timeEncoder, "polar_encoder", NULL, NULL);
  print_meas(&timeDecoder, "polar_decoder", NULL, NULL);
  loader_reset();
  if (logFlag)
    fclose(logFile);
  return ret;
}
