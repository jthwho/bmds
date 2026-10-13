# bmds_speech — text to speech (devplan/speech.md): SVOX Pico (vendored in thirdparty/svox-pico,
# patched; pico.cmake lists it) with its voices read in place from /rom.
#
# The speaker is portable: also built as a plain CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references saycommand.cpp by symbol -- its command is found through the
# linker section.

include(${CMAKE_CURRENT_LIST_DIR}/pico.cmake)

set(BMDS_SPEECH_PORTABLE_SRCS
        src/picopool.c
        src/speaker.cpp
        ${BMDS_PICO_SRCS})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_SPEECH_PORTABLE_SRCS}
                                    src/saycommand.cpp
                                    src/speechfile.cpp
                                    src/voices.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_PICO_INCLUDES}
                               REQUIRES bmds_core bmds_kernel bmds_fs
                               PRIV_REQUIRES bmds_audio bmds_shell bmds_sysconf
                               WHOLE_ARCHIVE)
        set(BMDS_SPEECH_TARGET ${COMPONENT_LIB})
else()
        add_library(bmds_speech STATIC ${BMDS_SPEECH_PORTABLE_SRCS})
        target_include_directories(bmds_speech PUBLIC include PRIVATE ${BMDS_PICO_INCLUDES})
        target_link_libraries(bmds_speech PUBLIC bmds_core)
        set(BMDS_SPEECH_TARGET bmds_speech)
endif()
bmds_target_options(${BMDS_SPEECH_TARGET})
target_compile_definitions(${BMDS_SPEECH_TARGET} PRIVATE ${BMDS_PICO_DEFINITIONS})
set_source_files_properties(${BMDS_PICO_SRCS} PROPERTIES COMPILE_OPTIONS "${BMDS_PICO_OPTIONS}")
set_source_files_properties(${BMDS_PICO_FAST_SRCS} PROPERTIES COMPILE_OPTIONS "-O2;${BMDS_PICO_OPTIONS}")
