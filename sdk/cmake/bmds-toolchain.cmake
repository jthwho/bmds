# bmds-toolchain.cmake: build BMDS programs with Espressif's RISC-V GCC.
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
#   cmake -S myprog -B build -DCMAKE_TOOLCHAIN_FILE=<bmds>/sdk/cmake/bmds-toolchain.cmake
#
# The compiler is riscv32-esp-elf-gcc from PATH (ESP-IDF's export.sh puts
# it there), else from ~/.espressif/tools. Then include BmdsProgram.cmake
# and use bmds_add_program().

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

find_program(BMDS_GCC riscv32-esp-elf-gcc)
if(NOT BMDS_GCC)
        file(GLOB _bmds_gccs "$ENV{HOME}/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc")
        if(_bmds_gccs)
                list(SORT _bmds_gccs)
                list(REVERSE _bmds_gccs)
                list(GET _bmds_gccs 0 BMDS_GCC)
        endif()
endif()
if(NOT BMDS_GCC)
        message(FATAL_ERROR "riscv32-esp-elf-gcc not found: run ESP-IDF's export.sh first")
endif()
get_filename_component(_bmds_bin "${BMDS_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER "${_bmds_bin}/riscv32-esp-elf-gcc")
set(CMAKE_CXX_COMPILER "${_bmds_bin}/riscv32-esp-elf-g++")
set(CMAKE_ASM_COMPILER "${_bmds_bin}/riscv32-esp-elf-gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Every P4 revision runs this (devplan/programs.md, D5): base, atomics,
# single-precision float, Espressif hardware loops and PIE SIMD. No Zc*.
set(BMDS_ARCH_FLAGS "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1 -mabi=ilp32f")
# PC-relative addressing (medany), no relaxation (gp is the kernel's).
set(BMDS_C_FLAGS "${BMDS_ARCH_FLAGS} -mcmodel=medany -mno-relax -ffunction-sections -fdata-sections")

set(CMAKE_C_FLAGS_INIT "${BMDS_C_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${BMDS_C_FLAGS} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "${BMDS_ARCH_FLAGS} -mno-relax")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
