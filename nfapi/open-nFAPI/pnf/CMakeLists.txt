# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(nfapi_pnf_lib
        src/pnf.c
        src/pnf_interface.c
        src/pnf_p7.c
        src/pnf_p7_interface.c)
target_link_libraries(nfapi_pnf_lib PRIVATE asn1_lte_rrc_hdrs asn1_nr_rrc_hdrs)
target_link_libraries(nfapi_pnf_lib PUBLIC nr_fapi_p7)
target_link_libraries(nfapi_pnf_lib PUBLIC nfapi_nr_lib nfapi_lte_lib)

if(OAI_WLS)
    target_compile_definitions(nfapi_pnf_lib PRIVATE ENABLE_WLS)
endif()
target_include_directories(nfapi_pnf_lib PUBLIC public_inc)
target_include_directories(nfapi_pnf_lib PUBLIC inc)
