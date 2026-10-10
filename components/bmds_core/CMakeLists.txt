# bmds_core — hardware-independent core types.
#
# Must not depend on ESP-IDF: this file is used both as an ESP-IDF
# component and as a plain CMake library by the host tests (tests/host).

set(BMDS_CORE_SRCS
        src/arena.cpp
        src/base64.cpp
        src/blake2s.cpp
        src/bytering.cpp
        src/chacha20.cpp
        src/clockmodel.cpp
        src/clipboardconversions.cpp
        src/clipboardtype.cpp
        src/color.cpp
        src/configtypes.cpp
        src/datetime.cpp
        src/desktopentry.cpp
        src/fft.cpp
        src/pngdecoder.cpp
        src/randompool.cpp
        src/romimage.cpp
        src/stat.cpp
        src/region.cpp
        src/error.cpp
        src/filenames.cpp
        src/fileformat.cpp
        src/iconpack.cpp
        src/mimeapps.cpp
        src/inflate.cpp
        src/inireader.cpp
        src/string.cpp
        src/timezone.cpp
        src/utf8.cpp
        src/vtparser.cpp
        src/zonebundle.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

# bmds/iconid.h, generated from rom/icons.list (devplan/rom.md §3f): the bmds icon theme's icons
# by number, for every component above this one.
set(BMDS_ICONS_LIST ${CMAKE_CURRENT_LIST_DIR}/../../rom/icons.list)
set(BMDS_MKROM ${CMAKE_CURRENT_LIST_DIR}/../../tools/mkrom.py)
set(BMDS_ICONID_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(BMDS_ICONID ${BMDS_ICONID_DIR}/bmds/iconid.h)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_CORE_SRCS}
                               INCLUDE_DIRS include)
        bmds_target_options(${COMPONENT_LIB})
        idf_build_get_property(BMDS_CORE_PYTHON PYTHON)
        set(BMDS_CORE_TARGET ${COMPONENT_LIB})
else()
        add_library(bmds_core STATIC ${BMDS_CORE_SRCS})
        target_include_directories(bmds_core PUBLIC include)
        target_compile_features(bmds_core PUBLIC cxx_std_20)
        bmds_target_options(bmds_core)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(BMDS_CORE_PYTHON ${Python3_EXECUTABLE})
        set(BMDS_CORE_TARGET bmds_core)
endif()
add_custom_command(OUTPUT ${BMDS_ICONID}
                   COMMAND ${BMDS_CORE_PYTHON} ${BMDS_MKROM} iconid ${BMDS_ICONS_LIST} ${BMDS_ICONID}
                   DEPENDS ${BMDS_MKROM} ${BMDS_ICONS_LIST}
                   VERBATIM)
target_sources(${BMDS_CORE_TARGET} PRIVATE ${BMDS_ICONID})
target_include_directories(${BMDS_CORE_TARGET} PUBLIC ${BMDS_ICONID_DIR})
