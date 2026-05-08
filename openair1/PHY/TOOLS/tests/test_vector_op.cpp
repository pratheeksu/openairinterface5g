/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <stdint.h>
#include <vector>
#include <algorithm>
#include <numeric>
extern "C" {
#include "openair1/PHY/TOOLS/tools_defs.h"
struct configmodule_interface_s;
struct configmodule_interface_s *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (assert) {
    abort();
  } else {
    exit(EXIT_SUCCESS);
  }
}
}
#include <cstdio>
#include "common/utils/LOG/log.h"
#include "openair1/PHY/TOOLS/phy_test_tools.hpp"

static float error_pct(c16_t fixed, cd_t ref) {
  float err_r = (float)fixed.r - (float)ref.r;
  float err_i = (float)fixed.i - (float)ref.i;
  float err_mag = sqrtf(err_r * err_r + err_i * err_i);
  float ref_mag = sqrtf(ref.r * ref.r + ref.i * ref.i);
  float denom = (ref_mag > 1.0f) ? ref_mag : 32767.0f; /* guard /0 */
  return 100.0f * err_mag / denom;
}

static int c16mul_tol_check(const char *label,
                     c16_t a, c16_t b, uint16_t q,
                     float tol_pct)
{
  /* fixed-point result */
  c16_t fp = c16mulShift(a, b, q);

  /* double reference – scale Q<q> inputs to [-1, 1] range */
  const double S = (double)((1 << q) - 1);
  cd_t ad = {a.r / S, a.i / S};
  cd_t bd = {b.r / S, b.i / S};
  cd_t rd = cdMul(ad, bd);

  /* scale reference back to Q<q> integer range for comparison */
  cd_t ref = {rd.r * S, rd.i * S};

  float pct = error_pct(fp, ref);

  if (pct > tol_pct)
    printf(
        "%-30s  a=(%6d,%6d)  b=(%6d,%6d)"
        "  got=(%6d,%6d)  ref=(%.1f,%.1f)  err=%.4f%%  %s\n",
        label,
        a.r,
        a.i,
        b.r,
        b.i,
        fp.r,
        fp.i,
        ref.r,
        ref.i,
        pct,
        pct > tol_pct ? "FAIL" : "PASS");

  return (pct > tol_pct);
}

int main()
{
  const float TOL = 1.0f;
  const uint16_t q = 15;
  int res = 0;

  const uint16_t maxq = (1 << q) - 1;
  const uint16_t one_sqrt2 = maxq * (1 / sqrt(2));
  const uint16_t halfq = maxq >> 1;

  /* unity × unity -> (1,0) */
  res |= c16mul_tol_check("unity x unity", (c16_t){maxq, 0}, (c16_t){maxq, 0}, q, TOL);

  /* unity × j -> (0,1) */
  res |= c16mul_tol_check("unity x j", (c16_t){maxq, 0}, (c16_t){0, maxq}, q, TOL);

  /* j × j -> (-1,0) */
  res |= c16mul_tol_check("j x j", (c16_t){0, maxq}, (c16_t){0, maxq}, q, TOL);

  /* conjugate pair -> purely real */
  res |= c16mul_tol_check("conjugate pair", (c16_t){one_sqrt2, one_sqrt2}, (c16_t){one_sqrt2, -one_sqrt2}, q, TOL);

  /* negative real × positive real */
  res |= c16mul_tol_check("neg x pos real", (c16_t){-maxq, 0}, (c16_t){halfq, 0}, q, TOL);

  /* small values (quantisation dominates) */
  res |= c16mul_tol_check("small values", (c16_t){100, 50}, (c16_t){200, -75}, q, TOL);

  /* full-scale worst case. The best we can do is 50% when saturating the result */
  res |= c16mul_tol_check("full scale", (c16_t){maxq, maxq}, (c16_t){maxq, maxq}, q, 51);

  /* zero inputs */
  res |= c16mul_tol_check("zero x anything", (c16_t){0, 0}, (c16_t){12345, -6789}, q, TOL);

  /* 45-degree phasor squared */
  res |= c16mul_tol_check("45deg phasor squared", (c16_t){one_sqrt2, one_sqrt2}, (c16_t){one_sqrt2, one_sqrt2}, q, TOL);
  res |= c16mul_tol_check("45deg phasor squared with noise",
                          (c16_t){one_sqrt2 + 100, -one_sqrt2},
                          (c16_t){one_sqrt2 + 100, -one_sqrt2},
                          q,
                          TOL);

  /* mixed signs */
  res |= c16mul_tol_check("mixed signs", (c16_t){-halfq, halfq}, (c16_t){halfq, -halfq}, q, TOL);

  if (res > 0)
    return 1;

  const int shift = 15; // it should always be 15 to keep int16 in same range
  for (int vector_size = 1237; vector_size < 1237 + 8; vector_size++) {
    auto input1 = generate_random_c16(vector_size);
    auto input2 = generate_random_c16(vector_size);

    // Clip the input for -32768 because this will make different result
    // C version promote the int16 to int before doing the conjugate (so negate imaginary part)
    // simd version do it on the int16, so the result overflows to -32768
    // other overflows exist, but they make the same
    // in C we do with int promotion, for real part:
    // (int)x.r*(int)y.r - (int)x.i*(int)y.i
    // that can overflow because each multiplication result is full int range
    // so when we sum, it overflows
    // _mm256_madd_epi16() overflows also, as do regular addition instruction
    // so the result will be the same (the same wrong value)

    for (auto it = input1.begin(); it != input1.end(); it++) {
      if (it->r == -32768)
        it->r = -32767;
      if (it->i == -32768)
        it->i = -32767;
    }
    for (auto it = input2.begin(); it != input2.end(); it++) {
      if (it->r == -32768)
        it->r = -32767;
      if (it->i == -32768)
        it->i = -32767;
    }

    AlignedVector512<c16_t> output;
    output.resize(vector_size);

    const int test_offset = 8;
    // Test multadd_cpx_vector()
    for (int off = 0; off < test_offset; off++) {
      std::fill(output.begin(), output.end(), c16_t{0, 0});
      multadd_cpx_vector(input1.data() + off, input2.data() + off, output.data() + off, vector_size - off, shift);
      for (int i = off; i < vector_size - off; i++) {
        const c16_t res = c16maddShift(input1[i], input2[i], (c16_t){0, 0}, shift);
        if (output[i].r != res.r || output[i].i != res.i) {
          printf("Error at %d, offset %d, size %d: (%d,%d) * (%d,%d) = (%d,%d) (should be (%d,%d))\n",
                 i,
                 off,
                 vector_size,
                 input1[i].r,
                 input1[i].i,
                 input2[i].r,
                 input2[i].i,
                 output[i].r,
                 output[i].i,
                 res.r,
                 res.i);
          return 1;
        }
      }
    }

    // Test mult_cpx_vector()
    for (int off = 0; off < test_offset; off++) {
      std::fill(output.begin(), output.end(), c16_t{0, 0});
      mult_cpx_vector(input1.data() + off, input2.data() + off, output.data() + off, vector_size - off, shift);
      for (int i = off; i < vector_size - off; i++) {
        const c16_t res = c16mulShift(input1[i], input2[i], shift);
        if (output[i].r != res.r || output[i].i != res.i) {
          printf("Error at %d, offset %d, size %d: (%d,%d) * (%d,%d) = (%d,%d) (should be (%d,%d))\n",
                 i,
                 off,
                 vector_size,
                 input1[i].r,
                 input1[i].i,
                 input2[i].r,
                 input2[i].i,
                 output[i].r,
                 output[i].i,
                 res.r,
                 res.i);
          return 1;
        }
      }
    }

    // Test multadd_cpx_vector_cpx_scalar()
    const c16_t alpha_cases[] = {
        {16384, 0}, // real only (0.5 in Q15)
        {0, 16384}, // imag only
        {23170, 23170}, // ~1/sqrt(2) + j/sqrt(2)
        {-16384, 8192}, // negative real
        {32767, 32767}, // near max
    };
    // Buffer start offset used to test unaligned inputs
    for (int off = 0; off < test_offset; off++) {
      const c16_t alpha = alpha_cases[off % sizeofArray(alpha_cases)];
      std::fill(output.begin(), output.end(), c16_t{0, 0});
      multadd_cpx_vector_cpx_scalar(input1.data() + off, alpha, output.data() + off, vector_size - off, shift);
      for (int i = off; i < vector_size - off; i++) {
        const c16_t res = c16maddShift(input1[i], alpha, (c16_t){0, 0}, shift);
        if (output[i].r != res.r || output[i].i != res.i) {
          printf("Error at %d, offset %d, size %d: (%d,%d) * (%d,%d) = (%d,%d) (should be (%d,%d))\n",
                 i,
                 off,
                 vector_size,
                 input1[i].r,
                 input1[i].i,
                 alpha.r,
                 alpha.i,
                 output[i].r,
                 output[i].i,
                 res.r,
                 res.i);
          return 1;
        }
      }
    }
  }
  return 0;
}
