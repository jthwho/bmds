# bmds_update — update packages checked and applied (devplan/update.md): the engine both
# firmwares share, its boot stage, and the `update` command. Target-only.
#
# WHOLE_ARCHIVE: nothing references updatecommand.cpp by symbol -- its command is found through
# the linker section.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/update.cpp
                            src/updatecommand.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel
                       PRIV_REQUIRES app_update bmds_console bmds_fs bmds_shell bmds_storage bmds_sysconf esp_hw_support esp_partition
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
