idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bmds_console bmds_kernel bmds_shell bmds_storage bmds_fs bmds_sysconf bmds_update
                                     esp_driver_i2c) # its settings are in ../sdkconfig.defaults

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)
bmds_target_options(${COMPONENT_LIB})
