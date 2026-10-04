# BmdsProgram.cmake: bmds_add_program(name SOURCES ... [STACK_SIZE bytes])
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds <name>.elf, a BMDS program (bmds/program.h), with the SDK's
# startup code, musl (BmdsLibc.cmake) and the SDK linker script, and <name>.debug.elf,
# the same program with all its symbols and debug information (for
# addr2line and GDB; <name>.elf keeps only what the loader needs). Use it
# with bmds-toolchain.cmake as the toolchain file.

get_filename_component(BMDS_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include(${CMAKE_CURRENT_LIST_DIR}/BmdsLibc.cmake)

# What every program compiles against: musl's headers (and GCC's own, for
# the intrinsics), never the toolchain's newlib; the SDK's bmds/*.h.
if(NOT TARGET bmds_crt)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
                        OUTPUT_VARIABLE BMDS_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
        add_library(bmds_headers INTERFACE)
        target_compile_options(bmds_headers INTERFACE -nostdinc)
        target_include_directories(bmds_headers SYSTEM INTERFACE ${BMDS_LIBC_INCLUDE} ${BMDS_GCC_INCLUDE})
        target_include_directories(bmds_headers INTERFACE ${BMDS_SDK_DIR}/include)
        # The startup code and the Linux system call layer musl runs on
        # (libbmds.a; _start is pulled in with -u).
        add_library(bmds_crt STATIC ${BMDS_SDK_DIR}/crt/start.c ${BMDS_SDK_DIR}/crt/linux.c)
        set_target_properties(bmds_crt PROPERTIES OUTPUT_NAME bmds ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_crt PUBLIC bmds_headers)
        target_compile_options(bmds_crt PRIVATE -Os -Wall -Wextra -Werror)
        add_custom_target(bmds_libs DEPENDS bmds_libc bmds_crt)
        # bmds-cc: the same compiler and link for other build systems
        # (CC=.../bmds-cc make), once bmds_libs is built.
        configure_file(${CMAKE_CURRENT_LIST_DIR}/bmds-cc.in ${CMAKE_BINARY_DIR}/bmds-cc
                       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
                                        WORLD_EXECUTE @ONLY)
endif()

function(bmds_add_program name)
        cmake_parse_arguments(ARG "" "STACK_SIZE" "SOURCES" ${ARGN})
        add_executable(${name} ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE bmds_headers -Wl,--start-group bmds_crt bmds_libc ${BMDS_LIBGCC}
                                      -Wl,--end-group)
        target_link_options(${name} PRIVATE
                # Linked at 0 with its relocations kept: the kernel moves it.
                -static -nostdlib -L${BMDS_LIBC_LIB} -u _start -Wl,--emit-relocs
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
