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
set(SYNTH ${BMDS_ROOT}/components/bmds_synth)
set(SYNTH_SRCS ${SYNTH}/src/synth.cpp ${SYNTH}/src/tsf.c ${SYNTH}/src/tml.c ${SYNTH}/src/mididecoder.cpp)
set(SYNTH_INCLUDES ${SYNTH}/include ${SYNTH}/src ${BMDS_ROOT}/thirdparty/tinysoundfont)
add_executable(decoder_fuzz decoder.cpp ${BMDS_ROOT}/tests/host/hostmemory.cpp ${BMDS_CODEC_SRCS}
                            ${AUDIO}/src/wavfile.cpp ${AUDIO}/src/sampleformat.cpp ${SYNTH_SRCS})
target_include_directories(decoder_fuzz PRIVATE ${BMDS_ROOT}/tests/host ${AUDIO}/include ${BMDS_CODEC_INCLUDES}
                                                ${SYNTH_INCLUDES})
target_compile_definitions(decoder_fuzz PRIVATE ${BMDS_CODEC_DEFINITIONS}
                                                BMDS_SYNTH_DIR="${BMDS_ROOT}/tests/host/synth")
target_compile_options(decoder_fuzz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_link_libraries(decoder_fuzz PRIVATE bmds_core)
target_link_options(decoder_fuzz PRIVATE -fsanitize=fuzzer)

# SoundFonts and the synth (devplan/audio.md §22), with stb_vorbis for SF3 fonts.
add_executable(soundfont_fuzz soundfont.cpp ${BMDS_ROOT}/tests/host/hostmemory.cpp ${SYNTH}/src/synth.cpp
                              ${SYNTH}/src/tsf.c ${AUDIO}/src/stbvorbis.c)
target_include_directories(soundfont_fuzz PRIVATE ${BMDS_ROOT}/tests/host ${SYNTH}/include ${SYNTH}/src
                                                  ${BMDS_ROOT}/thirdparty/tinysoundfont ${BMDS_ROOT}/thirdparty/stb_vorbis)
target_compile_definitions(soundfont_fuzz PRIVATE ${BMDS_CODEC_DEFINITIONS})
target_compile_options(soundfont_fuzz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_link_libraries(soundfont_fuzz PRIVATE bmds_core)
target_link_options(soundfont_fuzz PRIVATE -fsanitize=fuzzer)

# NTP's parser and arithmetic (devplan/network.md §22): bmds_net's portable ntp.cpp alone.
set(NET ${BMDS_ROOT}/components/bmds_net)
add_executable(ntp_fuzz ntp.cpp ${NET}/src/ntp.cpp)
target_include_directories(ntp_fuzz PRIVATE ${NET}/include)
target_compile_options(ntp_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(ntp_fuzz PRIVATE bmds_core)
target_link_options(ntp_fuzz PRIVATE -fsanitize=fuzzer)

# The leap-second list (devplan/network.md §31e): bmds_core's parser and SHA-1.
add_executable(leapseconds_fuzz leapseconds.cpp)
target_compile_options(leapseconds_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(leapseconds_fuzz PRIVATE bmds_core)
target_link_options(leapseconds_fuzz PRIVATE -fsanitize=fuzzer)

add_executable(updatepackage_fuzz updatepackage.cpp)
target_compile_options(updatepackage_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(updatepackage_fuzz PRIVATE bmds_core)
target_link_options(updatepackage_fuzz PRIVATE -fsanitize=fuzzer)

# PTP (devplan/network.md §31): the messages and an ordinary clock's port, bmds_net's portable
# ptp.cpp and ptpport.cpp.
add_executable(ptp_fuzz ptp.cpp ${NET}/src/ptp.cpp ${NET}/src/ptpport.cpp)
target_include_directories(ptp_fuzz PRIVATE ${NET}/include)
target_compile_options(ptp_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(ptp_fuzz PRIVATE bmds_core)
target_link_options(ptp_fuzz PRIVATE -fsanitize=fuzzer)

# The real-time classifier (devplan/network.md §30d): bmds_net's portable netclassifier.cpp and
# the framing it reads with.
add_executable(netclassifier_fuzz netclassifier.cpp ${NET}/src/netclassifier.cpp ${NET}/src/ethernet.cpp)
target_include_directories(netclassifier_fuzz PRIVATE ${NET}/include)
target_compile_options(netclassifier_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(netclassifier_fuzz PRIVATE bmds_core)
target_link_options(netclassifier_fuzz PRIVATE -fsanitize=fuzzer)

# Network MIDI (devplan/audio.md §23): RTP-MIDI's packets and AppleMIDI's sessions, bmds_midi's
# portable rtpmidi.cpp, applemidi.cpp and the parser.
set(MIDI ${BMDS_ROOT}/components/bmds_midi)
add_executable(rtpmidi_fuzz rtpmidi.cpp ${MIDI}/src/rtpmidi.cpp ${MIDI}/src/applemidi.cpp ${MIDI}/src/midiparser.cpp)
target_include_directories(rtpmidi_fuzz PRIVATE ${MIDI}/include)
target_compile_options(rtpmidi_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(rtpmidi_fuzz PRIVATE bmds_core)
target_link_options(rtpmidi_fuzz PRIVATE -fsanitize=fuzzer)

# Our SFTP server's protocol core (devplan/network.md §24): programs/sftp-server's sftp.c (C)
# with made-up file calls.
set(SFTP ${BMDS_ROOT}/programs/sftp-server)
add_executable(sftp_fuzz sftp.cpp ${SFTP}/sftp.c)
target_include_directories(sftp_fuzz PRIVATE ${SFTP})
target_compile_options(sftp_fuzz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_link_options(sftp_fuzz PRIVATE -fsanitize=fuzzer)

# The system log's text forms (devplan/logging.md): syslog messages as programs write them to
# /dev/log, from bmds_core's logtext.cpp.
add_executable(syslog_fuzz syslog.cpp)
target_compile_options(syslog_fuzz PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(syslog_fuzz PRIVATE bmds_core)
target_link_options(syslog_fuzz PRIVATE -fsanitize=fuzzer)

# Text to speech (devplan/speech.md): any text through the speaker on the en-US voice, Pico's
# sources with the settings bmds_speech gives them.
set(SPEECH ${BMDS_ROOT}/components/bmds_speech)
include(${SPEECH}/pico.cmake)
add_executable(speech_fuzz speech.cpp ${BMDS_ROOT}/tests/host/hostmemory.cpp ${SPEECH}/src/speaker.cpp
                           ${SPEECH}/src/picopool.c ${BMDS_PICO_SRCS})
target_include_directories(speech_fuzz PRIVATE ${BMDS_ROOT}/tests/host ${SPEECH}/include ${BMDS_PICO_INCLUDES})
target_compile_definitions(speech_fuzz PRIVATE ${BMDS_PICO_DEFINITIONS}
                                               BMDS_SPEECH_DIR="${BMDS_ROOT}/thirdparty/svox-pico/lang")
target_compile_options(speech_fuzz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
# Upstream's code: clang's warnings about it are no business here.
set_source_files_properties(${BMDS_PICO_SRCS} PROPERTIES COMPILE_OPTIONS "-fwrapv;-w")
target_link_libraries(speech_fuzz PRIVATE bmds_core)
target_link_options(speech_fuzz PRIVATE -fsanitize=fuzzer)
