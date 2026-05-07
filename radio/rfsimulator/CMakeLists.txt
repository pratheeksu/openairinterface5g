# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_compile_options(-Wunused-parameter)
add_library(rfsimulator MODULE
            simulator.cpp
            apply_channelmod.c
            ../../openair1/PHY/TOOLS/signal_energy.c
)
target_link_libraries(rfsimulator PRIVATE SIMU log_headers)
set_target_properties(rfsimulator PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(OAI_RFSIM_TAPS "Enable TAPS client support in RFSim" OFF)
if(OAI_RFSIM_TAPS)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/taps.fbs)
    file(DOWNLOAD
      "https://gitlab.eurecom.fr/oai/raytracing-channel-emulator/-/raw/59589ad2a534881810b2aa7dd81e63066245bb8d/server/api/taps.fbs?inline=false"
      ${CMAKE_SOURCE_DIR}/taps.fbs
    )
  endif()

  add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/taps_generated.h
    COMMAND flatc -c -o ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/taps.fbs
    DEPENDS ${CMAKE_SOURCE_DIR}/taps.fbs
  )

  find_package(FlatBuffers QUIET)
  if(NOT FlatBuffers_FOUND)
    find_package(Flatbuffers REQUIRED)
    set(FLATBUFFERS_TARGET flatbuffers)
  else()
    set(FLATBUFFERS_TARGET flatbuffers::flatbuffers)
  endif()

  if(NOT TARGET taps_client)
    add_library(rfsim_taps_client STATIC
      ../vrtsim/taps_client.cpp
      ${CMAKE_CURRENT_BINARY_DIR}/taps_generated.h
    )
    target_include_directories(rfsim_taps_client PUBLIC ../vrtsim ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(rfsim_taps_client PUBLIC ${FLATBUFFERS_TARGET} nanomsg SIMU)
    set(RFSIM_TAPS_LIB rfsim_taps_client)
  else()
    set(RFSIM_TAPS_LIB taps_client)
  endif()

  target_compile_definitions(rfsimulator PRIVATE OAI_RFSIM_TAPS_CLIENT=1)
  target_include_directories(rfsimulator PRIVATE ../vrtsim ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(rfsimulator PRIVATE ${RFSIM_TAPS_LIB})
endif()

add_executable(replay_node stored_node.c)
target_link_libraries (replay_node minimal_lib)
