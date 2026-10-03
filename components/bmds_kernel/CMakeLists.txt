# bmds_kernel — boot sequence, logging, memory spaces (and later: tasks/IPC
# wrappers, interrupt dispatch, device model, VFS).

idf_component_register(SRCS "src/assert.cpp"
                            "src/boot.cpp"
                            "src/esperr_check.cpp"
                            "src/log.cpp"
                            "src/memory.cpp"
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_board
                       PRIV_REQUIRES esp_app_format esp_hw_support esp_psram esp_timer freertos heap log
                       LDFRAGMENTS linker.lf)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)
bmds_target_options(${COMPONENT_LIB})
