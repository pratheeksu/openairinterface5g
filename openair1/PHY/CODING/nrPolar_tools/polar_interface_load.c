/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief load library implementing coding/decoding algorithms
 */

#include <stdlib.h>
#include <stdio.h>
#include "polar_interface.h"
#include "common/utils/LOG/log.h"
#include "common/utils/load_module_shlib.h"
#include "common/config/config_userapi.h"
#include "assertions.h"

// Arguments for simulator runs without the config module
char *polar_arguments_phy_simulators[64] = {"polartest", NULL};

int load_polar_interface(const char *version, polar_interface_t *itf) {
    char *ptr = (char *)config_get_if();
    char libname[64] = "polar";
    
    // If config module is not loaded (simulator mode)
    if (!ptr) {
        uniqCfg = load_configmodule(1, polar_arguments_phy_simulators, CONFIG_ENABLECMDLINEONLY);
        logInit();
    }
    
    // Function description array for the shared library
    loader_shlibfunc_t shlib_fdesc[] = {
        {.fname = "polar_init"},
        {.fname = "polar_shutdown"},
        {.fname = "polar_decoder"},
        {.fname = "polar_encoder"},

    };

    int ret = load_module_version_shlib(libname, (char *)version, shlib_fdesc, sizeofArray(shlib_fdesc), NULL);
    if (ret < 0) {
        fprintf(stderr, "Polar module unavailable\n");
        return ret;
    }

    itf->polar_init             = (polar_init_t *)shlib_fdesc[0].fptr;
    itf->polar_shutdown         = (polar_shutdown_t *)shlib_fdesc[1].fptr;
    itf->polar_decoder = (polar_dec_t *)shlib_fdesc[2].fptr;
    itf->polar_encoder = (polar_enc_t *)shlib_fdesc[3].fptr;
    if (itf->polar_init)
        AssertFatal(itf->polar_init() == 0, "error starting polar library %s %s\n", libname, version);
 
    return 0;
}

int free_polar_interface(polar_interface_t *itf)
{
  if (itf && itf->polar_shutdown)
    return itf->polar_shutdown();
  return 0;
}
