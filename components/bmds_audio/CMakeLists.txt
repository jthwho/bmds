# bmds_audio — audio devices (devplan/audio.md): the ES8311 codec, I2S output,
# the device's filling task, and the `audio` command.
#
# The controls, the ES8311's register arithmetic, the tone generator, the
# sample formats, the resampler, the limiter and the streams are portable: also built as a
# plain CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references audiocommand.cpp by symbol -- its command
# is found through the linker section.

set(BMDS_AUDIO_PORTABLE_SRCS
        src/audiocontrol.cpp
        src/audiomix.cpp
        src/audiostream.cpp
        src/es8311regs.cpp
        src/resampler.cpp
        src/sampleformat.cpp
        src/sinetone.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_AUDIO_PORTABLE_SRCS}
                                    src/audiocommand.cpp
                                    src/audiodevice.cpp
                                    src/es8311.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel bmds_board bmds_drivers bmds_sysconf
                               PRIV_REQUIRES bmds_shell esp_driver_gpio esp_driver_i2s esp_hw_support esp_mm
                                             esp_timer
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
        # The engine's inner loops: -O2 (the build's -Os leaves them several times slower).
        set_source_files_properties(src/audiomix.cpp src/audiostream.cpp src/resampler.cpp src/sampleformat.cpp
                                    src/sinetone.cpp PROPERTIES COMPILE_OPTIONS -O2)
else()
        add_library(bmds_audio STATIC ${BMDS_AUDIO_PORTABLE_SRCS})
        target_include_directories(bmds_audio PUBLIC include)
        target_link_libraries(bmds_audio PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_audio)
endif()
