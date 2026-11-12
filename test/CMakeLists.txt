# Swell's host tests: the portable core (src/core) built for the computer.
#
#   cmake -S test -B build-host && cmake --build build-host
#   build-host/swell_core_test && build-host/swell_fields_test && build-host/swell_proto_test
#   node test/editor_codec_test.mjs build-host/swell_blobdump
#
# (`make test` at the top runs all three.)
cmake_minimum_required(VERSION 3.20)
project(swell_tests C)
set(CMAKE_C_STANDARD 99)
set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../src/core)
set(CORE ${CORE_DIR}/model.c ${CORE_DIR}/store.c ${CORE_DIR}/engine.c ${CORE_DIR}/out.c ${CORE_DIR}/proto.c ${CORE_DIR}/fields.c)

# store, engine timelines, MIDI out
add_executable(swell_core_test core_test.c ${CORE})
# the settings pages' fields
add_executable(swell_fields_test fields_test.c ${CORE})
# the SysEx editor protocol
add_executable(swell_proto_test proto_test.c ${CORE})
# C-encoded blobs for the editor's JS codec test
add_executable(swell_blobdump blobdump.c ${CORE})
foreach(t swell_core_test swell_fields_test swell_proto_test swell_blobdump)
    target_include_directories(${t} PRIVATE ${CORE_DIR})
    target_compile_options(${t} PRIVATE -O2 -Wall -Wextra)
endforeach()
