# bmds_bench — the system's benchmarks in one place (devplan/bench.md): the table every
# component registers its benchmarks in (BMDS_BENCH, a linker section), the context they measure
# with, and the `bench` command, which runs them and writes a JSON file a run.
#
# Its pure part (BenchText: the statistics, patterns, file names) is also built as a plain CMake
# library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references the command by symbol -- it is found through the linker
# section.

set(BMDS_BENCH_PURE_SRCS
        src/benchtext.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_BENCH_PURE_SRCS}
                                    src/benchcommand.cpp
                                    src/benchprograms.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core
                               PRIV_REQUIRES bmds_fs bmds_kernel bmds_shell bmds_sysconf
                               LDFRAGMENTS linker.lf
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_bench STATIC ${BMDS_BENCH_PURE_SRCS})
        target_include_directories(bmds_bench PUBLIC include)
        target_link_libraries(bmds_bench PUBLIC bmds_core)
        bmds_target_options(bmds_bench)
endif()
