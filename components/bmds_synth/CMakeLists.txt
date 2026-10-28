# bmds_synth — the General MIDI synths (devplan/audio.md §22, §26): SoundFonts and the synth on
# TinySoundFont (vendored in thirdparty/tinysoundfont, patched), the FM synth on OP2 banks, synth
# fds and the fonts they share, the `synth` command.
#
# WHOLE_ARCHIVE: nothing references synthcommand.cpp or benchsynth.cpp by symbol -- the command
# and the benchmarks are found through their linker sections.
#
# The font, the synths, the bank and the MIDI file decoder are portable: also built as a plain
# CMake library for the host tests (tests/host). SF3 fonts decode through stb_vorbis, whose
# code is bmds_audio's (its settings are the same here).

set(BMDS_SYNTH_PORTABLE_SRCS
        src/fmsynth.cpp
        src/mididecoder.cpp
        src/op2bank.cpp
        src/synth.cpp
        src/synthlimiter.cpp
        src/tml.c
        src/tsf.c)
set(BMDS_SYNTH_THIRDPARTY ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty)
set(BMDS_SYNTH_INCLUDES ${BMDS_SYNTH_THIRDPARTY}/tinysoundfont ${BMDS_SYNTH_THIRDPARTY}/stb_vorbis)
set(BMDS_SYNTH_DEFINITIONS STB_VORBIS_IO STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_SYNTH_PORTABLE_SRCS}
                                    src/benchsynth.cpp
                                    src/synthcommand.cpp
                                    src/synthfile.cpp
                                    src/synthservice.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_SYNTH_INCLUDES}
                               REQUIRES bmds_core bmds_kernel bmds_audio bmds_fs bmds_sysconf
                               PRIV_REQUIRES bmds_bench bmds_midi bmds_shell
                               WHOLE_ARCHIVE)
        set(BMDS_SYNTH_TARGET ${COMPONENT_LIB})
else()
        add_library(bmds_synth STATIC ${BMDS_SYNTH_PORTABLE_SRCS})
        target_include_directories(bmds_synth PUBLIC include PRIVATE ${BMDS_SYNTH_INCLUDES})
        target_link_libraries(bmds_synth PUBLIC bmds_core bmds_kernel bmds_audio)
        set(BMDS_SYNTH_TARGET bmds_synth)
endif()
bmds_target_options(${BMDS_SYNTH_TARGET})
target_compile_definitions(${BMDS_SYNTH_TARGET} PRIVATE ${BMDS_SYNTH_DEFINITIONS})
# The render loop: -O2 (the build's -Os is slower on inner loops); TinySoundFont is upstream code.
set_source_files_properties(src/synth.cpp src/fmsynth.cpp src/synthlimiter.cpp PROPERTIES COMPILE_OPTIONS -O2)
set_source_files_properties(src/tsf.c src/tml.c PROPERTIES COMPILE_OPTIONS "-O2;-Wno-shadow;-Wno-unused-parameter;-Wno-sign-compare")
