idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bmds_kernel)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/bmds.cmake)
bmds_target_options(${COMPONENT_LIB})
