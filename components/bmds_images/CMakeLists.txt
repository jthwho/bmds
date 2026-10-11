# bmds_images — the system's pictures (devplan/images.md §3d): one front over the decoders (PNG,
# BMP and TJpgDec in bmds_core, the JPEG codec in bmds_drivers), encoding as PNG or JPEG, and the
# `image` command. The SDK's image_* calls are this. The pure parts (the decoders, the plan, the
# scaler) are bmds_core's, host-tested there.
#
# WHOLE_ARCHIVE: nothing references imagecommand.cpp by symbol -- its command is found through the
# linker section.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/images.cpp
                            src/imagecommand.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_fs bmds_kernel
                       PRIV_REQUIRES bmds_drivers bmds_shell bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
