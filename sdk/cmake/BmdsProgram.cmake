# BmdsProgram.cmake: bmds_add_program(name SOURCES ... [STACK_SIZE bytes])
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds <name>.elf, a BMDS program (bmds/program.h), with the SDK's
# startup code, picolibc and the SDK linker script. Use it with
# bmds-toolchain.cmake as the toolchain file.

get_filename_component(BMDS_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET bmds_crt)
        enable_language(ASM)
        add_library(bmds_crt OBJECT ${BMDS_SDK_DIR}/crt/crt0.S ${BMDS_SDK_DIR}/crt/crt.c ${BMDS_SDK_DIR}/crt/malloc.c)
        target_include_directories(bmds_crt PUBLIC ${BMDS_SDK_DIR}/include)
        target_compile_options(bmds_crt PRIVATE -O2 -Wall -Wextra -Werror)
endif()

function(bmds_add_program name)
        cmake_parse_arguments(ARG "" "STACK_SIZE" "SOURCES" ${ARGN})
        add_executable(${name} ${ARG_SOURCES} $<TARGET_OBJECTS:bmds_crt>)
        target_include_directories(${name} PRIVATE ${BMDS_SDK_DIR}/include)
        target_link_options(${name} PRIVATE
                # Linked at 0 with its relocations kept: the kernel moves it.
                -static -nostartfiles -Wl,--emit-relocs
                -T ${BMDS_SDK_DIR}/ld/bmds.ld
                -Wl,--no-relax -Wl,--gc-sections
                -Wl,-z,max-page-size=16 -Wl,-z,common-page-size=16
                -Wl,--no-warn-rwx-segments)
        if(ARG_STACK_SIZE)
                target_link_options(${name} PRIVATE -Wl,--defsym=__bmds_stack_size=${ARG_STACK_SIZE})
        endif()
        set_target_properties(${name} PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${BMDS_SDK_DIR}/ld/bmds.ld)
endfunction()
