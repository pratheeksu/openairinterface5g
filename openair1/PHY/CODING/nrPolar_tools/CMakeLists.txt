# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(polar MODULE
  nr_polar_init.c
  nr_bitwise_operations.c
  nr_crc_byte.c
  nr_polar_decoder.c
  nr_polar_decoding_tools.c
  nr_polar_encoder.c
  nr_polar_interleaving_pattern.c
  nr_polar_kernal_operation.c
  nr_polar_kronecker_power_matrices.c
  nr_polar_matrix_and_array.c
  nr_polar_procedures.c
  nr_polar_sequence_pattern.c
)

target_link_libraries(polar PRIVATE utils crc_byte)
set_target_properties(polar PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(polartest polar)
add_dependencies(nr-softmodem polar)
add_dependencies(nr-uesoftmodem polar)
add_dependencies(nr_ulsim polar)
add_dependencies(nr_ulschsim polar)
add_dependencies(nr_dlsim polar)
add_dependencies(nr_dlschsim polar)

add_library(polar_load OBJECT polar_interface_load.c)
target_link_libraries(polar_load PRIVATE log_headers)
