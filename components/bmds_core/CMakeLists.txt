# bmds_core — hardware-independent core types.
#
# Must not depend on ESP-IDF: this file is used both as an ESP-IDF
# component and as a plain CMake library by the host tests (tests/host).

set(BMDS_CORE_SRCS
        src/arena.cpp
        src/bytering.cpp
        src/clockmodel.cpp
        src/color.cpp
        src/datetime.cpp
        src/region.cpp
        src/error.cpp
        src/string.cpp
        src/timezone.cpp
        src/utf8.cpp
        src/vtparser.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_CORE_SRCS}
                               INCLUDE_DIRS include)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_core STATIC ${BMDS_CORE_SRCS})
        target_include_directories(bmds_core PUBLIC include)
        target_compile_features(bmds_core PUBLIC cxx_std_20)
        bmds_target_options(bmds_core)
endif()
