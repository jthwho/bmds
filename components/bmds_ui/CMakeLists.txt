# bmds_ui — the desktop and its windows (devplan/windows.md).
#
# The pure parts (the compose plan, fonts) are also built as a plain CMake
# library for the host tests (tests/host). The font's glyph table is
# generated from thirdparty/unscii by tools/fontconv.py.

set(BMDS_UI_PURE_SRCS
        src/compose.cpp
        src/font.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

set(BMDS_UI_FONT_HEX ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/unscii/unscii-16.hex)
set(BMDS_UI_FONTCONV ${CMAKE_CURRENT_LIST_DIR}/../../tools/fontconv.py)
set(BMDS_UI_FONT_SRC ${CMAKE_CURRENT_BINARY_DIR}/font_unscii16.cpp)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_UI_PURE_SRCS}
                               INCLUDE_DIRS include
                               REQUIRES bmds_core)
        idf_build_get_property(BMDS_UI_PYTHON PYTHON)
        set(BMDS_UI_LIB ${COMPONENT_LIB})
else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(BMDS_UI_PYTHON ${Python3_EXECUTABLE})
        add_library(bmds_ui STATIC ${BMDS_UI_PURE_SRCS})
        target_include_directories(bmds_ui PUBLIC include)
        target_link_libraries(bmds_ui PUBLIC bmds_core)
        set(BMDS_UI_LIB bmds_ui)
endif()

add_custom_command(OUTPUT ${BMDS_UI_FONT_SRC}
                   COMMAND ${BMDS_UI_PYTHON} ${BMDS_UI_FONTCONV} ${BMDS_UI_FONT_HEX} ${BMDS_UI_FONT_SRC} unscii16
                   DEPENDS ${BMDS_UI_FONTCONV} ${BMDS_UI_FONT_HEX}
                   VERBATIM)
target_sources(${BMDS_UI_LIB} PRIVATE ${BMDS_UI_FONT_SRC})
bmds_target_options(${BMDS_UI_LIB})
