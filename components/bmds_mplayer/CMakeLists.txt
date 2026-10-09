# bmds_mplayer — /bin/mplayer, the media player (devplan/mediaplayer.md): sound and MIDI files,
# playlists, a library by tags, a visualizer and a MIDI view; an ordinary GUI program on the
# public toolkit and the kernel's public classes, built into the firmware only to have it at hand.
# Nothing in the OS refers to it.
#
# Its pure parts (the playlist, M3U, the spectrum's bands, the MIDI channels, the library's
# index) are also built as a plain CMake library for the host tests (tests/host).

set(BMDS_MPLAYER_PURE_SRCS
        src/libraryindex.cpp
        src/m3u.cpp
        src/midichannels.cpp
        src/playlist.cpp
        src/spectrum.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_MPLAYER_PURE_SRCS}
                               PRIV_REQUIRES bmds_core bmds_fs bmds_kernel bmds_synth)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_mplayer STATIC ${BMDS_MPLAYER_PURE_SRCS})
        target_include_directories(bmds_mplayer PUBLIC src)
        target_link_libraries(bmds_mplayer PUBLIC bmds_core bmds_fs bmds_kernel bmds_synth)
        bmds_target_options(bmds_mplayer)
endif()
