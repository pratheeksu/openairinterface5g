# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(ldpc_load OBJECT nrLDPC_coding_interface_load.c)
target_link_libraries(ldpc_load PRIVATE log_headers)

add_subdirectory(nrLDPC_coding_segment)
add_subdirectory(nrLDPC_coding_aal)
