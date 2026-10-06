# BMDS host unit tests (plain CMake, no ESP-IDF).
#
#   cmake -S tests/host -B build-host
#   cd build-host && build && ctest --output-on-failure

cmake_minimum_required(VERSION 3.22)
project(bmds_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(BMDS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${BMDS_ROOT}/components/bmds_core ${CMAKE_BINARY_DIR}/bmds_core)
add_subdirectory(${BMDS_ROOT}/components/bmds_kernel ${CMAKE_BINARY_DIR}/bmds_kernel)
add_subdirectory(${BMDS_ROOT}/components/bmds_console ${CMAKE_BINARY_DIR}/bmds_console)
add_subdirectory(${BMDS_ROOT}/components/bmds_shell ${CMAKE_BINARY_DIR}/bmds_shell)
add_subdirectory(${BMDS_ROOT}/components/bmds_fs ${CMAKE_BINARY_DIR}/bmds_fs)
add_subdirectory(${BMDS_ROOT}/components/bmds_loader ${CMAKE_BINARY_DIR}/bmds_loader)
add_subdirectory(${BMDS_ROOT}/components/bmds_drivers ${CMAKE_BINARY_DIR}/bmds_drivers)
add_subdirectory(${BMDS_ROOT}/components/bmds_input ${CMAKE_BINARY_DIR}/bmds_input)
add_subdirectory(${BMDS_ROOT}/components/bmds_ui ${CMAKE_BINARY_DIR}/bmds_ui)

add_executable(bmds_host_tests
        main.cpp
        hostmemory.cpp
        arena.cpp
        blockdevice.cpp
        bytering.cpp
        edid.cpp
        futex.cpp
        memtest.cpp
        error.cpp
        fdtable.cpp
        files.cpp
        input.cpp
        interactive.cpp
        interrupt.cpp
        language.cpp
        launch.cpp
        lineeditor.cpp
        list.cpp
        loader.cpp
        memorystream.cpp
        memspace.cpp
        pipe.cpp
        process.cpp
        regex.cpp
        result.cpp
        run.cpp
        pty.cpp
        screenconsole.cpp
        shell.cpp
        signals.cpp
        span.cpp
        staticobject.cpp
        string.cpp
        stringview.cpp
        syntax.cpp
        terminal.cpp
        terminalscreen.cpp
        ui.cpp
        time.cpp
        vfs.cpp
        workerpool.cpp
        ymodem.cpp)
target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROOT}/thirdparty/doctest)
target_compile_definitions(bmds_host_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
        BMDS_CORPUS_DIR="${CMAKE_CURRENT_LIST_DIR}/corpus"
        BMDS_COMPAT_DIR="${CMAKE_CURRENT_LIST_DIR}/../compat")
target_link_libraries(bmds_host_tests PRIVATE bmds_core bmds_kernel bmds_console bmds_shell bmds_fs bmds_loader bmds_drivers bmds_input bmds_ui)
bmds_target_options(bmds_host_tests)

# bmds-elfcheck: the loader's format check as a host tool (tools/elfcheck).
add_executable(bmds-elfcheck ${BMDS_ROOT}/tools/elfcheck/main.cpp hostmemory.cpp)
target_link_libraries(bmds-elfcheck PRIVATE bmds_loader)
bmds_target_options(bmds-elfcheck)

enable_testing()
add_test(NAME bmds_host_tests COMMAND bmds_host_tests)

# The SDK examples, cross-compiled when Espressif's RISC-V GCC is around
# (on PATH, or in ~/.espressif): the tests then load the real thing.
find_program(BMDS_RISCV_GCC riscv32-esp-elf-gcc)
if(NOT BMDS_RISCV_GCC)
        file(GLOB _bmds_gccs "$ENV{HOME}/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc")
        if(_bmds_gccs)
                list(GET _bmds_gccs 0 BMDS_RISCV_GCC)
        endif()
endif()
if(BMDS_RISCV_GCC)
        include(ExternalProject)
        ExternalProject_Add(sdk_examples
                SOURCE_DIR ${BMDS_ROOT}/sdk/examples
                BINARY_DIR ${CMAKE_BINARY_DIR}/sdk-examples
                CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${BMDS_ROOT}/sdk/cmake/bmds-toolchain.cmake
                           -DCMAKE_BUILD_TYPE=MinSizeRel -DBMDS_GCC=${BMDS_RISCV_GCC}
                BUILD_ALWAYS ON
                INSTALL_COMMAND "")
        add_dependencies(bmds_host_tests sdk_examples)
        target_compile_definitions(bmds_host_tests PRIVATE
                BMDS_SDK_HELLO="${CMAKE_BINARY_DIR}/sdk-examples/hello.elf"
                BMDS_SDK_EXAMPLES="${CMAKE_BINARY_DIR}/sdk-examples")
        add_test(NAME sdk_examples_are_programs
                 COMMAND bmds-elfcheck ${CMAKE_BINARY_DIR}/sdk-examples/hello.elf)
else()
        message(STATUS "riscv32-esp-elf-gcc not found: the SDK examples are not built or tested")
endif()

# The corpus's expectations, checked against real bash (when there is one).
find_program(BASH_PROGRAM bash)
find_package(Python3 COMPONENTS Interpreter)
if(BASH_PROGRAM AND Python3_Interpreter_FOUND)
        add_test(NAME corpus_agrees_with_bash
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/corpus/check_bash.py
                         ${CMAKE_CURRENT_LIST_DIR}/corpus)
endif()
