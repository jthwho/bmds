# BMDS fuzz targets (libFuzzer, with AddressSanitizer and UndefinedBehaviorSanitizer):
#
#   CC=clang CXX=clang++ cmake -S tests/fuzz -B build-fuzz -G Ninja
#   cd build-fuzz && build
#   ./decoder_fuzz -jobs=3 -max_len=65536 corpus ../tests/host/audio    (README.md)
#
# The components are built from their host branch (as for tests/host), every file instrumented.

cmake_minimum_required(VERSION 3.22)
project(bmds_fuzz LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "tests/fuzz needs clang (libFuzzer): CC=clang CXX=clang++ cmake ...")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(BMDS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# Every object instrumented; the fuzzer's main linked into the targets alone.
# GCC, the only compiler of the firmware, defines left shifts of negative values (as an extension):
# stb_vorbis has them, so UBSan is told not to report those alone.
add_compile_options(-g -O2 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined
                    -fno-sanitize=shift-base)
add_link_options(-fsanitize=address,undefined)

add_subdirectory(${BMDS_ROOT}/components/bmds_core ${CMAKE_BINARY_DIR}/bmds_core)
# Warnings are the GCC builds' business (clang finds others); here only faults count.
target_compile_options(bmds_core PRIVATE -Wno-error)

# The decoders (devplan/audio.md §19): every format, through Decoder. Their sources alone, with the
# settings bmds_audio gives them (not the rest of bmds_audio, which clang can't build: GCC's
# constexpr std::sin).
set(AUDIO ${BMDS_ROOT}/components/bmds_audio)
include(${AUDIO}/codecs.cmake)
list(TRANSFORM BMDS_CODEC_SRCS PREPEND ${AUDIO}/)
add_executable(decoder_fuzz decoder.cpp ${BMDS_ROOT}/tests/host/hostmemory.cpp ${BMDS_CODEC_SRCS}
                            ${AUDIO}/src/wavfile.cpp ${AUDIO}/src/sampleformat.cpp)
target_include_directories(decoder_fuzz PRIVATE ${BMDS_ROOT}/tests/host ${AUDIO}/include ${BMDS_CODEC_INCLUDES})
target_compile_definitions(decoder_fuzz PRIVATE ${BMDS_CODEC_DEFINITIONS})
target_compile_options(decoder_fuzz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_link_libraries(decoder_fuzz PRIVATE bmds_core)
target_link_options(decoder_fuzz PRIVATE -fsanitize=fuzzer)
