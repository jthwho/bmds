# bmds_storage — the SD card driver and the storage service (card detect,
# hot-plug), and the `sd` command. Target-only.
#
# WHOLE_ARCHIVE: nothing references sdcommand.cpp by symbol -- its command is
# found through the linker section -- so without it the linker would never
# pull that object out of the archive.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/sdcard.cpp
                            src/sdcommand.cpp
                            src/storage.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel bmds_fs
                       PRIV_REQUIRES bmds_board bmds_shell esp_driver_gpio esp_driver_sdmmc esp_hw_support
                                     esp_mm sdmmc
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
