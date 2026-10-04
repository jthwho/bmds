# BmdsProgram.cmake: bmds_add_program(name SOURCES ... [STACK_SIZE bytes])
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds <name>.elf, a BMDS program (bmds/program.h), with the SDK's
# startup code, picolibc and the SDK linker script, and <name>.debug.elf,
# the same program with all its symbols and debug information (for
# addr2line and GDB; <name>.elf keeps only what the loader needs). Use it
# with bmds-toolchain.cmake as the toolchain file.

get_filename_component(BMDS_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET bmds_crt)
        enable_language(ASM)
        add_library(bmds_crt OBJECT ${BMDS_SDK_DIR}/crt/crt0.S ${BMDS_SDK_DIR}/crt/crt.c ${BMDS_SDK_DIR}/crt/malloc.c
                    ${BMDS_SDK_DIR}/crt/posix.c)
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
                # The segment starts on a 512-byte boundary in the file: the
                # loader's read of it is then whole sectors, straight into the
                # (4096-aligned) image.
                -Wl,-z,max-page-size=512 -Wl,-z,common-page-size=512
                -Wl,--no-warn-rwx-segments)
        if(ARG_STACK_SIZE)
                target_link_options(${name} PRIVATE -Wl,--defsym=__bmds_stack_size=${ARG_STACK_SIZE})
        endif()
        set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}.debug" SUFFIX ".elf"
                                                 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                                                 LINK_DEPENDS ${BMDS_SDK_DIR}/ld/bmds.ld)
        # What goes on the card: symbols no relocation needs and debug
        # information removed (a tenth of the size, and faster to load).
        get_filename_component(_bmds_bin "${CMAKE_C_COMPILER}" DIRECTORY)
        add_custom_command(TARGET ${name} POST_BUILD
                COMMAND ${_bmds_bin}/riscv32-esp-elf-objcopy --strip-unneeded
                        $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.elf
                BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${name}.elf
                COMMENT "Stripping ${name}.elf")
endfunction()
