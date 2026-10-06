# bmds_console — the terminal abstraction, the line editor and the consoles.
#
# The interface and the line editor are pure C++ (no ESP-IDF headers) and are
# also built as a plain CMake library for the host tests (tests/host). The
# console drivers are target-only.

set(BMDS_CONSOLE_PURE_SRCS
        src/colorfilter.cpp
        src/console.cpp
        src/lineeditor.cpp
        src/pty.cpp
        src/screenconsole.cpp
        src/terminalscreen.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_CONSOLE_PURE_SRCS}
                                    src/usbserialconsole.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board esp_hw_support hal soc)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_console STATIC ${BMDS_CONSOLE_PURE_SRCS})
        target_include_directories(bmds_console PUBLIC include)
        target_link_libraries(bmds_console PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_console)
endif()
