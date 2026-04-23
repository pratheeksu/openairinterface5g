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
            nrLDPC_coding_cuda.c

            nrLDPC_decoder_cuda.c
            ldpc_encoder_cuda32.c
            ldpc_input32.c

            nrLDPC_coding_segment_encoder_cuda.c
            nrLDPC_coding_segment_decoder_cuda.c

            # .cu files
            nrLDPC_decoder_BG1_cuda.cu
            ldpc_BG1_Zc384_32bit.cu
            ldpc_input.cu
            ldpc_deinterleave.cu
            ldpc_rate_matching.cu
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
