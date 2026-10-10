# bmds_sysconf — the system configuration (devplan/sysconf.md): the registry of typed parameters,
# their saved values on /boot, watchers, /home following `home`, and the clock's zone and zone
# database. The `sysconf` command is the shell's (bmds_shell), which sits above this.
#
# Portable: also built as a plain CMake library for the host tests (tests/host), without the boot
# stage (sysconfinit.cpp), which logs.
#
# WHOLE_ARCHIVE: the parameters are found through their linker section (BMDS_SYSCONF), and a file
# may hold only them.

set(BMDS_SYSCONF_SRCS
        src/sysconf.cpp
        src/sysconfhome.cpp
        src/sysconfwatch.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_SYSCONF_SRCS} src/sysconfinit.cpp src/sysconfsystem.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS src
                               REQUIRES bmds_core bmds_kernel bmds_fs
                               LDFRAGMENTS linker.lf
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_sysconf STATIC ${BMDS_SYSCONF_SRCS})
        target_include_directories(bmds_sysconf PUBLIC include PRIVATE src)
        target_link_libraries(bmds_sysconf PUBLIC bmds_core bmds_kernel bmds_fs)
        bmds_target_options(bmds_sysconf)
endif()
