# bmds_midi — MIDI (devplan/audio.md §21): ports, the router between them,
# /dev/midiN, the `midi` command and its settings; MIDI over the network (§23).
#
# The parser, the event ring, the router, the USB packer, RTP-MIDI's packets and AppleMIDI's
# sessions are portable: also built as a
# plain CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references midicommand.cpp by symbol -- its command
# is found through the linker section.

set(BMDS_MIDI_PORTABLE_SRCS
        src/applemidi.cpp
        src/midiparser.cpp
        src/midirouter.cpp
        src/midiusbpacker.cpp
        src/rtpmidi.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_MIDI_PORTABLE_SRCS}
                                    src/midi.cpp
                                    src/midicommand.cpp
                                    src/midifile.cpp
                                    src/netmidi.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel bmds_fs bmds_sysconf
                               PRIV_REQUIRES bmds_net bmds_shell
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_midi STATIC ${BMDS_MIDI_PORTABLE_SRCS})
        target_include_directories(bmds_midi PUBLIC include)
        target_link_libraries(bmds_midi PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_midi)
endif()
