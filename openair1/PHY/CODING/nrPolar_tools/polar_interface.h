/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief interface for libraries implementing coding/decoding algorithms
 */
#ifndef __POLAR_INTERFACE__H__
#define __POLAR_INTERFACE__H__

#include <stdint.h>
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"


typedef int32_t(polar_init_t)(void);
typedef int32_t(polar_shutdown_t)(void);

typedef void(polar_enc_t)(uint64_t *in, void *out, polar_type_t type, uint16_t len, uint8_t aggregation_level, uint32_t crcmask);

typedef uint32_t(polar_dec_t)(int16_t *in, uint64_t *out, polar_type_t type, uint16_t len, uint8_t aggregation_level);

typedef struct polar_interface_s {
  polar_init_t *polar_init;
  polar_shutdown_t *polar_shutdown;
  polar_dec_t *polar_decoder;
  polar_enc_t *polar_encoder;
} polar_interface_t;

// Functions to load and free the Polar library
int load_polar_interface(const char *version, polar_interface_t *itf);
int free_polar_interface(polar_interface_t *itf);

polar_dec_t polar_decoder;
polar_enc_t polar_encoder;

#endif
