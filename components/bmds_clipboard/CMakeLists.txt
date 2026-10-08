# bmds_clipboard — the clipboard service, /dev/clip0 and `clip` (devplan/clipboard.md).
#
# All but the boot stage and the command's registration are portable: also built as a plain
# CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references clipboardinit.cpp's command by symbol -- it is found
# through the linker section.

set(BMDS_CLIPBOARD_PORTABLE_SRCS
        src/clipboard.cpp
        src/clipboardreader.cpp
        src/clipcommand.cpp
        src/clipfiles.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_CLIPBOARD_PORTABLE_SRCS}
                                    src/clipboardinit.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel bmds_shell
                               PRIV_REQUIRES bmds_fs bmds_sysconf
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_clipboard STATIC ${BMDS_CLIPBOARD_PORTABLE_SRCS})
        target_include_directories(bmds_clipboard PUBLIC include)
        target_link_libraries(bmds_clipboard PUBLIC bmds_core bmds_kernel bmds_shell bmds_fs)
        bmds_target_options(bmds_clipboard)
endif()
