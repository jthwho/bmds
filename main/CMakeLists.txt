idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bmds_console bmds_drivers bmds_kernel bmds_shell bmds_storage bmds_fs bmds_loader)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/bmds.cmake)
bmds_target_options(${COMPONENT_LIB})
