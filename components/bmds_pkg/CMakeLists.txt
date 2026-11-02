# bmds_pkg — packages (devplan/packages.md): installing, removing and recording them, and the `pkg`
# command. The engine and the command are portable (the VFS and bmds_core) and also built as a
# plain CMake library for the host tests; the settings, the keys and the command's registration
# are target-only.
#
# WHOLE_ARCHIVE: nothing references pkgsystem.cpp by symbol -- its command and settings are found
# through linker sections.

set(BMDS_PKG_PURE_SRCS
        src/keys.cpp
        src/packages.cpp
        src/pkgcommand.cpp
        src/pkgrecord.cpp
        src/repositories.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_PKG_PURE_SRCS}
                                    src/fetchsystem.cpp
                                    src/pkgsystem.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_fs bmds_shell
                               PRIV_REQUIRES bmds_kernel bmds_loader bmds_net bmds_sysconf
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_pkg STATIC ${BMDS_PKG_PURE_SRCS})
        target_include_directories(bmds_pkg PUBLIC include)
        target_link_libraries(bmds_pkg PUBLIC bmds_core bmds_fs bmds_shell)
        bmds_target_options(bmds_pkg)
endif()
