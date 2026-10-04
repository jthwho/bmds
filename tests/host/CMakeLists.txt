# BMDS host unit tests (plain CMake, no ESP-IDF).
#
#   cmake -S tests/host -B build-host
#   cd build-host && build && ctest --output-on-failure

cmake_minimum_required(VERSION 3.22)
project(bmds_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(BMDS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${BMDS_ROOT}/components/bmds_core ${CMAKE_BINARY_DIR}/bmds_core)
add_subdirectory(${BMDS_ROOT}/components/bmds_kernel ${CMAKE_BINARY_DIR}/bmds_kernel)
add_subdirectory(${BMDS_ROOT}/components/bmds_console ${CMAKE_BINARY_DIR}/bmds_console)
add_subdirectory(${BMDS_ROOT}/components/bmds_shell ${CMAKE_BINARY_DIR}/bmds_shell)

add_executable(bmds_host_tests
        main.cpp
        hostmemory.cpp
        arena.cpp
        bytering.cpp
        error.cpp
        fdtable.cpp
        interactive.cpp
        interrupt.cpp
        language.cpp
        lineeditor.cpp
        list.cpp
        memorystream.cpp
        memspace.cpp
        pipe.cpp
        regex.cpp
        result.cpp
        run.cpp
        shell.cpp
        span.cpp
        staticobject.cpp
        string.cpp
        stringview.cpp
        syntax.cpp
        workerpool.cpp)
target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROOT}/thirdparty/doctest)
target_compile_definitions(bmds_host_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
        BMDS_CORPUS_DIR="${CMAKE_CURRENT_LIST_DIR}/corpus")
target_link_libraries(bmds_host_tests PRIVATE bmds_core bmds_kernel bmds_console bmds_shell)
bmds_target_options(bmds_host_tests)

enable_testing()
add_test(NAME bmds_host_tests COMMAND bmds_host_tests)

# The corpus's expectations, checked against real bash (when there is one).
find_program(BASH_PROGRAM bash)
find_package(Python3 COMPONENTS Interpreter)
if(BASH_PROGRAM AND Python3_Interpreter_FOUND)
        add_test(NAME corpus_agrees_with_bash
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/corpus/check_bash.py
                         ${CMAKE_CURRENT_LIST_DIR}/corpus)
endif()
