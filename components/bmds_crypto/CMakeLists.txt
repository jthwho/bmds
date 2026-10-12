# bmds_crypto — the chip's AES and SHA engines for the kernel and for programs (slot 112;
# devplan/network.md §26). Target only: it is all hardware.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS src/crypto.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core
                               PRIV_REQUIRES esp_security esp_hw_support esp_mm hal soc bmds_kernel)
        bmds_target_options(${COMPONENT_LIB})
endif()
