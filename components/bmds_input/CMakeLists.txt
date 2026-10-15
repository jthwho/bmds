# bmds_input — USB host, keyboards and mice (devplan/input.md), gamepads (devplan/controls.md),
# USB MIDI devices (devplan/audio.md §21).
#
# The pure parts (key events, the keymap and report decoder, keys as
# terminal bytes, HID descriptors, pads' layouts and mappings, actions; ProgramControls on the
# kernel's host build) are also built as a plain CMake library for the host tests
# (tests/host).
#
# WHOLE_ARCHIVE: nothing references usbcommand.cpp or padcommand.cpp by symbol -- their
# commands are found through the linker section.

set(BMDS_INPUT_PURE_SRCS
        src/chord.cpp
        src/controls.cpp
        src/hiddescriptor.cpp
        src/keyboarddecoder.cpp
        src/mouselayout.cpp
        src/padlayout.cpp
        src/padmapping.cpp
        src/programcontrols.cpp
        src/terminalkeys.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_INPUT_PURE_SRCS}
                                    src/input.cpp
                                    src/padcommand.cpp
                                    src/pads.cpp
                                    src/usbcommand.cpp
                                    src/usbhost.cpp
                                    src/usbmidi.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_fs bmds_midi bmds_sysconf bmds_shell bmds_tinyusb esp_driver_gpio esp_hw_support
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_input STATIC ${BMDS_INPUT_PURE_SRCS})
        target_include_directories(bmds_input PUBLIC include)
        target_link_libraries(bmds_input PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_input)
endif()
