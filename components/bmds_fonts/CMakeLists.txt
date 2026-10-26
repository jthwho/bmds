# bmds_fonts — the font library (devplan/fonts.md): the bitmap fonts built in (unscii's glyph table
# generated from thirdparty/unscii by tools/fontconv.py, the icons), TrueType faces through
# stb_truetype (vendored in thirdparty/stb_truetype) checked before it reads them; the font.render
# bench.
#
# WHOLE_ARCHIVE: nothing references benchfonts.cpp by symbol -- its bench is found through the
# linker section.
#
# The faces are portable: also built as a plain CMake library for the host tests (tests/host) and
# the fuzzer (tests/fuzz), where stb_truetype's own asserts are on.

set(BMDS_FONTS_PORTABLE_SRCS
        src/bitmapfont.cpp
        src/font.cpp
        src/fontface.cpp
        src/fontlibrary.cpp
        src/fontname.cpp
        src/glyphcache.cpp
        src/iconfont.cpp
        src/stbtruetype.c)
set(BMDS_FONTS_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/stb_truetype)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_FONTS_PORTABLE_SRCS}
                                    src/benchfonts.cpp
                                    src/systemfonts.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_FONTS_INCLUDES}
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_bench bmds_fs bmds_sysconf
                               WHOLE_ARCHIVE)
        set(BMDS_FONTS_TARGET ${COMPONENT_LIB})
        idf_build_get_property(BMDS_FONTS_PYTHON PYTHON)
else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(BMDS_FONTS_PYTHON ${Python3_EXECUTABLE})
        add_library(bmds_fonts STATIC ${BMDS_FONTS_PORTABLE_SRCS})
        target_include_directories(bmds_fonts PUBLIC include PRIVATE ${BMDS_FONTS_INCLUDES})
        target_link_libraries(bmds_fonts PUBLIC bmds_core bmds_kernel)
        target_compile_definitions(bmds_fonts PRIVATE BMDS_STBTT_ASSERTS)
        set(BMDS_FONTS_TARGET bmds_fonts)
endif()
set(BMDS_FONTS_UNSCII_HEX ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/unscii/unscii-16.hex)
set(BMDS_FONTS_FONTCONV ${CMAKE_CURRENT_LIST_DIR}/../../tools/fontconv.py)
set(BMDS_FONTS_UNSCII_SRC ${CMAKE_CURRENT_BINARY_DIR}/font_unscii16.cpp)
add_custom_command(OUTPUT ${BMDS_FONTS_UNSCII_SRC}
                   COMMAND ${BMDS_FONTS_PYTHON} ${BMDS_FONTS_FONTCONV} ${BMDS_FONTS_UNSCII_HEX} ${BMDS_FONTS_UNSCII_SRC}
                           unscii16
                   DEPENDS ${BMDS_FONTS_FONTCONV} ${BMDS_FONTS_UNSCII_HEX}
                   VERBATIM)
target_sources(${BMDS_FONTS_TARGET} PRIVATE ${BMDS_FONTS_UNSCII_SRC})
bmds_target_options(${BMDS_FONTS_TARGET})
# -O2 (the build's -Os is slower on inner loops): the kerning's binary searches (half the time:
# fonts.md §6a), layout and the cache's hits (§6c), and the rasterizer; stb_truetype is upstream code.
set_source_files_properties(src/fontface.cpp src/font.cpp src/glyphcache.cpp PROPERTIES COMPILE_OPTIONS -O2)
set_source_files_properties(src/stbtruetype.c PROPERTIES COMPILE_OPTIONS
                            "-O2;-Wno-shadow;-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-function")
