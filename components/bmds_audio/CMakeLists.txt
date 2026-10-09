# bmds_audio — audio devices (devplan/audio.md): the ES8311 codec, I2S output,
# the device's filling task, /dev/dsp, and the `audio` and `play` commands.
#
# The controls, the ES8311's register arithmetic, the tone generator, the
# sample formats, the resampler, the limiter, the streams, the WAV reader and
# the decoders are portable: also built as a plain CMake library for the host
# tests (tests/host).
#
# The decoders' libraries are vendored in thirdparty/ (minimp3, stb_vorbis,
# dr_flac): codecs.cmake lists them, with their settings.
#
# WHOLE_ARCHIVE: nothing references audiocommand.cpp or playcommand.cpp by
# symbol -- their commands are found through the linker section.

include(${CMAKE_CURRENT_LIST_DIR}/codecs.cmake)

set(BMDS_AUDIO_PORTABLE_SRCS
        src/audiocontrol.cpp
        src/audiomix.cpp
        src/audiostream.cpp
        src/es8311regs.cpp
        src/resampler.cpp
        src/sampleformat.cpp
        src/sinetone.cpp
        src/wavfile.cpp
        src/decodedfile.cpp
        ${BMDS_CODEC_SRCS})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_AUDIO_PORTABLE_SRCS}
                                    src/audiocommand.cpp
                                    src/audiodevice.cpp
                                    src/audiofile.cpp
                                    src/es8311.cpp
                                    src/playcommand.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_CODEC_INCLUDES}
                               REQUIRES bmds_core bmds_kernel bmds_board bmds_drivers bmds_fs bmds_sysconf
                               PRIV_REQUIRES bmds_shell esp_driver_gpio esp_driver_i2s esp_hw_support esp_mm
                                             esp_timer
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
        # The engine's inner loops: -O2 (the build's -Os leaves them several times slower).
        set_source_files_properties(src/audiomix.cpp src/audiostream.cpp src/resampler.cpp src/sampleformat.cpp
                                    src/sinetone.cpp ${BMDS_CODEC_FAST_SRCS} PROPERTIES COMPILE_OPTIONS -O2)
        # stb_vorbis's own code shadows variables.
        set_source_files_properties(src/stbvorbis.c PROPERTIES COMPILE_OPTIONS "-O2;-Wno-shadow")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${BMDS_CODEC_DEFINITIONS})
else()
        add_library(bmds_audio STATIC ${BMDS_AUDIO_PORTABLE_SRCS})
        target_include_directories(bmds_audio PUBLIC include PRIVATE ${BMDS_CODEC_INCLUDES})
        target_link_libraries(bmds_audio PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_audio)
        target_compile_definitions(bmds_audio PRIVATE ${BMDS_CODEC_DEFINITIONS})
        set_source_files_properties(src/stbvorbis.c PROPERTIES COMPILE_OPTIONS -Wno-shadow)
endif()
