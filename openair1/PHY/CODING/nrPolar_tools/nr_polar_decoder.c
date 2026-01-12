/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*
 * Return values:
 *  0 --> Success
 * -1 --> All list entries have failed the CRC checks
 */

#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/polar_interface.h"
#include "assertions.h"


static inline void nr_polar_rate_matching_int16(int16_t *input,
                                                int16_t *output,
                                                const uint16_t *rmp,
                                                const uint16_t K,
                                                const uint16_t N,
                                                const uint16_t E,
                                                const uint8_t i_bil)
{
  if (E >= N) { // repetition
    memset(output, 0, N * sizeof(*output));
    for (int i = 0; i <= E - 1; i++)
      output[rmp[i]] += input[i];
  } else {
    if ((K / (double)E) <= (7.0 / 16))
      memset(output, 0, N * sizeof(*output)); // puncturing
    else { // shortening
      for (int i = 0; i <= N - 1; i++)
        output[i] = INT16_MAX;
    }

    for (int i = 0; i <= E - 1; i++)
      output[rmp[i]] = input[i];
  }
}

static inline void nr_polar_info_extraction_from_u(uint64_t *Cprime,
                                                   const uint8_t *u,
                                                   const uint8_t *information_bit_pattern,
                                                   const uint8_t *parity_check_bit_pattern,
                                                   const uint16_t *interleaving_pattern,
                                                   uint16_t N,
                                                   uint8_t n_pc,
                                                   int K)
{
  int k = 0;

  if (n_pc > 0) {
    for (int n = 0; n < N; n++) {
      if (information_bit_pattern[n] == 1 && parity_check_bit_pattern[n] == 0) {
        int targetBit = K - 1 - interleaving_pattern[k];
        int k1 = targetBit >> 6;
        int k2 = targetBit & 63;
        Cprime[k1] |= (uint64_t)u[n] << k2;
        k++;
      }
    }
  } else {
    for (int n = 0; n < N; n++) {
      if (information_bit_pattern[n] == 1) {
        int targetBit = K - 1 - interleaving_pattern[k];
        int k1 = targetBit >> 6;
        int k2 = targetBit & 63;
        Cprime[k1] |= (uint64_t)u[n] << k2;
        k++;
      }
    }
  }
}
uint32_t polar_decoder(int16_t *input,
                             uint64_t *out,
                             polar_type_t messageType,
                             uint16_t messageLength,
                             uint8_t aggregation_level)
{
  t_nrPolar_params *polarParams = nr_polar_params(messageType, messageLength, aggregation_level);
  const uint N = polarParams->N;
#ifdef POLAR_CODING_DEBUG
  printf("\nRX\n");
  printf("rm:");
  for (int i = 0; i < N; i++) {
    if (i % 4 == 0) {
      printf(" ");
    }
    printf("%i", input[i] < 0 ? 1 : 0);
  }
  printf("\n");
#endif

  int16_t d_tilde[N];
  const uint E = polarParams->encoderLength;
  int16_t inbis[E];
  int16_t *input_deinterleaved;
  if (polarParams->consts.i_bil) {
    for (int i = 0; i < E; i++)
      inbis[i] = input[polarParams->i_bil_pattern[i]];
    input_deinterleaved = inbis;
  } else {
    input_deinterleaved = input;
  }

  nr_polar_rate_matching_int16(input_deinterleaved,
                               d_tilde,
                               polarParams->rate_matching_pattern,
                               polarParams->K,
                               N,
                               E,
                               polarParams->consts.i_bil);
#ifdef POLAR_CODING_DEBUG
  printf("d: ");
  for (int i = 0; i < N; i++) {
    if (i % 4 == 0) {
      printf(" ");
    }
    printf("%i", d_tilde[i] < 0 ? 1 : 0);
  }
  printf("\n");
#endif
  uint8_t nr_polar_U[N];
  memset(nr_polar_U, 0, sizeof(nr_polar_U));
  memcpy(treeAlpha(polarParams->decoder.root), d_tilde, sizeof(d_tilde));
  generic_polar_decoder(polarParams, polarParams->decoder.root, nr_polar_U);
#ifdef POLAR_CODING_DEBUG
  printf("u: ");
  for (int i = 0; i < N; i++) {
    if (i % 4 == 0) {
      printf(" ");
    }
    printf("%i", nr_polar_U[i]);
  }
  printf("\n");
#endif

  // Extract the information bits (û to ĉ)
  uint64_t B[4] = {0};
  nr_polar_info_extraction_from_u(B,
                                  nr_polar_U,
                                  polarParams->information_bit_pattern,
                                  polarParams->parity_check_bit_pattern,
                                  polarParams->interleaving_pattern,
                                  N,
                                  polarParams->n_pc,
                                  polarParams->K);

#ifdef POLAR_CODING_DEBUG
  printf("c: ");
  for (int n = 0; n < polarParams->K; n++) {
    if (n % 4 == 0) {
      printf(" ");
    }
    int n1 = n >> 6;
    int n2 = n - (n1 << 6);
    printf("%lu", (B[n1] >> n2) & 1);
  }
  printf("\n");
#endif

  int len = polarParams->payloadBits;
  // int len_mod64=len&63;
  int crclen = polarParams->crcParityBits;
  uint64_t rxcrc = B[0] & ((1 << crclen) - 1);
  uint32_t crc = 0;
  uint64_t Ar = 0;
  AssertFatal(len < 65, "A must be less than 65 bits\n");

  // appending 24 ones before a0 for DCI as stated in 38.212 7.3.2
  uint8_t offset = 0;
  if (messageType == NR_POLAR_DCI_MESSAGE_TYPE)
    offset = 3;

  if (len <= 32) {
    Ar = (uint32_t)(B[0] >> crclen);
    uint8_t A32_flip[4 + offset];
    if (messageType == NR_POLAR_DCI_MESSAGE_TYPE) {
      A32_flip[0] = 0xff;
      A32_flip[1] = 0xff;
      A32_flip[2] = 0xff;
    }
    uint32_t Aprime = (uint32_t)(Ar << (32 - len));
    A32_flip[0 + offset] = ((uint8_t *)&Aprime)[3];
    A32_flip[1 + offset] = ((uint8_t *)&Aprime)[2];
    A32_flip[2 + offset] = ((uint8_t *)&Aprime)[1];
    A32_flip[3 + offset] = ((uint8_t *)&Aprime)[0];
    if (crclen == 24)
      crc = (uint64_t)((crc24c(A32_flip, 8 * offset + len) >> 8) & 0xffffff);
    else if (crclen == 11)
      crc = (uint64_t)((crc11(A32_flip, 8 * offset + len) >> 21) & 0x7ff);
    else if (crclen == 6)
      crc = (uint64_t)((crc6(A32_flip, 8 * offset + len) >> 26) & 0x3f);
  } else if (len <= 64) {
    Ar = (B[0] >> crclen) | (B[1] << (64 - crclen));
    uint8_t A64_flip[8 + offset];
    if (messageType == NR_POLAR_DCI_MESSAGE_TYPE) {
      A64_flip[0] = 0xff;
      A64_flip[1] = 0xff;
      A64_flip[2] = 0xff;
    }
    uint64_t Aprime = (uint64_t)(Ar << (64 - len));
    A64_flip[0 + offset] = ((uint8_t *)&Aprime)[7];
    A64_flip[1 + offset] = ((uint8_t *)&Aprime)[6];
    A64_flip[2 + offset] = ((uint8_t *)&Aprime)[5];
    A64_flip[3 + offset] = ((uint8_t *)&Aprime)[4];
    A64_flip[4 + offset] = ((uint8_t *)&Aprime)[3];
    A64_flip[5 + offset] = ((uint8_t *)&Aprime)[2];
    A64_flip[6 + offset] = ((uint8_t *)&Aprime)[1];
    A64_flip[7 + offset] = ((uint8_t *)&Aprime)[0];
    if (crclen == 24)
      crc = (uint64_t)(crc24c(A64_flip, 8 * offset + len) >> 8) & 0xffffff;
    else if (crclen == 11)
      crc = (uint64_t)(crc11(A64_flip, 8 * offset + len) >> 21) & 0x7ff;
    else if (crclen == 6)
      crc = (uint64_t)(crc6(A64_flip, 8 * offset + len) >> 26) & 0x3f;
  }

#ifdef POLAR_CODING_DEBUG
  int A_array = (len + 63) >> 6;
  printf("a: ");
  for (int n = 0; n < len; n++) {
    if (n % 4 == 0) {
      printf(" ");
    }
    int n1 = n >> 6;
    int n2 = n - (n1 << 6);
    int alen = n1 == 0 ? len - (A_array << 6) : 64;
    printf("%lu", (Ar >> (alen - 1 - n2)) & 1);
  }
  printf("\n\n");
#endif

#ifdef POLAR_CODING_DEBUG
  printf("A %lx B %lx|%lx Cprime %lx|%lx (crc %x,rxcrc %lx, XOR %lx, bits%d)\n",
         Ar,
         B[1],
         B[0],
         Cprime[1],
         Cprime[0],
         crc,
         rxcrc,
         crc ^ rxcrc,
         polarParams->payloadBits);
#endif

  out[0] = Ar;

  polarReturn(polarParams);
  return crc ^ rxcrc;
}
