# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(coding MODULE
            3gpplte_sse.c
            3gpplte.c
            3gpplte_turbo_decoder_sse_8bit.c
            3gpplte_turbo_decoder_sse_16bit.c
            3gpplte_turbo_decoder_avx2_16bit.c
            3gpplte_turbo_decoder.c
)
set_target_properties(coding PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(coding PRIVATE log_headers)

add_library(ldpc_orig MODULE
            nrLDPC_decoder/nrLDPC_decoder.c
            nrLDPC_encoder/ldpc_encoder.c
)
set_target_properties(ldpc_orig PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(ldpc_orig PRIVATE ldpc_segment ldpc_gen_HEADERS)

add_library(ldpc MODULE
            nrLDPC_decoder/nrLDPC_decoder.c
            nrLDPC_encoder/ldpc_encoder_optim8segmulti.c
)
set_target_properties(ldpc PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(ldpc PRIVATE ldpc_segment ldpc_gen_HEADERS)

add_dependencies(ldpctest ldpc ldpc_orig)
add_dependencies(nr-softmodem ldpc ldpc_orig)
add_dependencies(nr-uesoftmodem ldpc ldpc_orig)
add_dependencies(nr_ulsim ldpc ldpc_orig)
add_dependencies(nr_ulschsim ldpc ldpc_orig)
add_dependencies(nr_dlsim ldpc ldpc_orig)
add_dependencies(nr_dlschsim ldpc ldpc_orig)

##############################################
# Base CUDA setting
##############################################

add_boolean_option(ENABLE_LDPC_CUDA OFF "Build support for CUDA" OFF)
if (ENABLE_LDPC_CUDA)
  enable_language(CUDA)

  # it would be possible to auto-detect compute capailities: https://stackoverflow.com/a/68223399
  #include(FindCUDA/select_compute_arch)
  #CUDA_DETECT_INSTALLED_GPUS(INSTALLED_GPU_CCS_1)

  # Nvidia recommends to build against all targeted archs: https://forums.developer.nvidia.com/t/261162
  # this is for cmake 3.16 (OAI minimum), starting at cmake 3.18 there is another syntax, see below

  SET(CUDA_VERBOSE_BUILD ON)

  # version using NVLink-C2C in Grace Hopper
  add_library(ldpc_cuda MODULE
              # main entry
              nrLDPC_coding/nrLDPC_coding_cuda.c

              nrLDPC_decoder/nrLDPC_decoder_cuda.c
              nrLDPC_encoder/ldpc_encoder_cuda32.c
              nrLDPC_encoder/ldpc_input32.c

              nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_encoder_cuda.c
              nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_decoder_cuda.c

              # .cu files
              nrLDPC_decoder/nrLDPC_decoder_BG1_cuda.cu
              nrLDPC_encoder/ldpc_BG1_Zc384_32bit.cu
              nrLDPC_encoder/ldpc_input.cu
              nrLDPC_coding/nrLDPC_coding_segment/ldpc_deinterleave.cu
              nrLDPC_coding/nrLDPC_coding_segment/ldpc_rate_matching.cu
  )
  set_target_properties(ldpc_cuda PROPERTIES CUDA_ARCHITECTURES "80;87;90")
  # we need to find the right include directory, but ti seems that
  # enable_language(CUDA) does not make this available(?), so also use the CUDA package
  find_package(CUDA REQUIRED)
  target_include_directories(ldpc_cuda PRIVATE ${CUDA_INCLUDE_DIRS})
  
  target_link_libraries(ldpc_cuda ldpc_gen_HEADERS log_headers)
  set_target_properties(ldpc_cuda PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  set_target_properties(ldpc_cuda PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  add_dependencies(ldpctest ldpc_cuda)
  add_dependencies(nr-softmodem ldpc_cuda)
  add_dependencies(nr-uesoftmodem ldpc_cuda)
  add_dependencies(nr_ulsim ldpc_cuda)
  add_dependencies(nr_ulschsim ldpc_cuda)
  add_dependencies(nr_dlsim ldpc_cuda)
  add_dependencies(nr_dlschsim ldpc_cuda)

  target_compile_definitions(ldpc_cuda PRIVATE USE_CUDA)

endif()

add_library(crc_byte OBJECT crc_byte.c)

add_subdirectory(nrLDPC_coding)
add_subdirectory(nrLDPC_decoder)
add_subdirectory(nrPolar_tools)
add_subdirectory(nrSmallBlock)
