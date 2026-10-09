# The decoders and their vendored libraries (devplan/audio.md §19): sources, include directories and
# settings, shared by this component's CMakeLists.txt and tests/fuzz.
#
# The libraries' settings are compile definitions, seen alike by their own files and by the decoders
# that include their headers (thirdparty/*/README.md).

set(BMDS_THIRDPARTY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty)
set(BMDS_CODEC_SRCS
        src/decoder.cpp
        src/wavdecoder.cpp
        src/mp3decoder.cpp
        src/vorbisdecoder.cpp
        src/flacdecoder.cpp
        src/minimp3.c
        src/stbvorbis.c
        src/drflac.c)
set(BMDS_CODEC_INCLUDES ${BMDS_THIRDPARTY_DIR}/minimp3 ${BMDS_THIRDPARTY_DIR}/stb_vorbis ${BMDS_THIRDPARTY_DIR}/dr_flac)
set(BMDS_CODEC_DEFINITIONS
        MINIMP3_FLOAT_OUTPUT MINIMP3_NO_SIMD
        STB_VORBIS_IO STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API
        DR_FLAC_NO_STDIO DR_FLAC_NO_OGG DR_FLAC_NO_SIMD)
# The decoders' inner loops at -O2 (measured: -Os decodes 15 to 21 % slower).
set(BMDS_CODEC_FAST_SRCS src/mp3decoder.cpp src/vorbisdecoder.cpp src/flacdecoder.cpp src/minimp3.c src/drflac.c)
