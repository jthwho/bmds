# BmdsProgram.cmake: bmds_add_program(name SOURCES ... [STACK_SIZE bytes] [ICONS png...] [DISPLAY])
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds <name>.elf, a BMDS program (bmds/program.h), with the SDK's
# startup code, musl (BmdsLibc.cmake) and the SDK linker script, and <name>.debug.elf,
# the same program with all its symbols and debug information (for
# addr2line and GDB; <name>.elf keeps only what the loader needs). Use it
# with bmds-toolchain.cmake as the toolchain file.
#
# ICONS: PNG images (16x16 and 32x32 are the sizes shown) built into the
# program as BMDS_NOTE_ICON notes (bmds/program.h), so that launchers and
# title bars show it with no icon file. The project needs the ASM language.
#
# DISPLAY: it draws to the screen continuously (a game, a demo): BMDS_PROGRAM_DISPLAY, and the
# kernel starts it on the core the display's own work isn't on.

get_filename_component(BMDS_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Debug information for everything built from here on (the SDK's libraries, musl, programs):
# <name>.debug.elf then has the files and lines tools/bmds-fault and GDB show. GCC's code is the
# same with it, and <name>.elf, on the card, is stripped of it (devplan/protection.md §22).
add_compile_options(-g)

# Paths by fixed names (devplan/debuginfo.md §3a): the debug information and __FILE__ name files as
# if the project were /, with the SDK at /sdk, the build at /build and the toolchain's headers at
# /riscv32-esp-elf, never where this machine keeps them, so a program built from the same sources
# with the same toolchain has the same build-id in any folder on any machine. Absolute, so tools
# don't join them to the (mapped) compile directory; tools/bmds-fault --source-root puts a real
# root before them. GCC uses the last map that matches, so the longest (innermost) path goes last;
# a project inside the SDK (its examples) keeps the SDK's, its real path there.
get_filename_component(_bmds_toolchain_root "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(_bmds_toolchain_root "${_bmds_toolchain_root}/.." ABSOLUTE)
set(_bmds_maps "${BMDS_SDK_DIR}=/sdk" "${CMAKE_BINARY_DIR}=/build" "${_bmds_toolchain_root}=/riscv32-esp-elf")
string(FIND "${CMAKE_SOURCE_DIR}/" "${BMDS_SDK_DIR}/" _bmds_in_sdk)
if(NOT _bmds_in_sdk EQUAL 0)
        list(APPEND _bmds_maps "${CMAKE_SOURCE_DIR}/=/")
endif()
set(_bmds_sorted "")
foreach(_bmds_map IN LISTS _bmds_maps)
        string(FIND "${_bmds_map}" "=" _bmds_eq REVERSE)
        math(EXPR _bmds_key "100000 + ${_bmds_eq}") # the path's length, padded to sort as text
        list(APPEND _bmds_sorted "${_bmds_key}|${_bmds_map}")
endforeach()
list(SORT _bmds_sorted)
set(BMDS_CC_PREFIX_MAPS "") # bmds-cc's, the same
foreach(_bmds_entry IN LISTS _bmds_sorted)
        string(REGEX REPLACE "^[0-9]+\\|" "" _bmds_map "${_bmds_entry}")
        add_compile_options("-ffile-prefix-map=${_bmds_map}")
        string(APPEND BMDS_CC_PREFIX_MAPS " -ffile-prefix-map=${_bmds_map}")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/BmdsLibc.cmake)

# bmds-link (sdk/tools) runs every program's link.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# What every program compiles against: musl's headers (and GCC's own, for
# the intrinsics), never the toolchain's newlib; the SDK's bmds/*.h.
if(NOT TARGET bmds_crt)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
                        OUTPUT_VARIABLE BMDS_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
        add_library(bmds_headers INTERFACE)
        target_compile_options(bmds_headers INTERFACE -nostdinc)
        # C++ (sdk/README.md, "C++"): the SDK's freestanding headers before them (<new>,
        # <type_traits>...): programs have no libstdc++.
        target_include_directories(bmds_headers SYSTEM INTERFACE $<$<COMPILE_LANGUAGE:CXX>:${BMDS_SDK_DIR}/include/c++>)
        target_include_directories(bmds_headers SYSTEM INTERFACE ${BMDS_LIBC_INCLUDE} ${BMDS_GCC_INCLUDE})
        target_include_directories(bmds_headers INTERFACE ${BMDS_SDK_DIR}/include)
        # The startup code and the Linux system call layer musl runs on
        # (libbmds.a; _start is pulled in with -u).
        add_library(bmds_crt STATIC ${BMDS_SDK_DIR}/crt/start.c ${BMDS_SDK_DIR}/crt/linux.c ${BMDS_SDK_DIR}/crt/oss.c
                                    ${BMDS_SDK_DIR}/crt/app.c ${BMDS_SDK_DIR}/crt/cxx.c
                                    ${BMDS_SDK_DIR}/crt/vfork.S ${BMDS_SDK_DIR}/crt/syscall_cp.S)
        set_target_properties(bmds_crt PROPERTIES OUTPUT_NAME bmds ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_crt PUBLIC bmds_headers)
        target_compile_options(bmds_crt PRIVATE -Os -Wall -Wextra -Werror)
        # libbmdsgame.a (bmds/game.h, devplan/games.md §13): what games share; an archive, so a
        # program that uses none of it gets none.
        add_library(bmds_game STATIC ${BMDS_SDK_DIR}/game/canvas.c ${BMDS_SDK_DIR}/game/text.c
                                     ${BMDS_SDK_DIR}/game/loop.c
                                     ${BMDS_SDK_DIR}/game/convert.c ${BMDS_SDK_DIR}/game/image.c
                                     ${BMDS_SDK_DIR}/game/mix.c ${BMDS_SDK_DIR}/game/audio.c
                                     ${BMDS_SDK_DIR}/game/smf.c ${BMDS_SDK_DIR}/game/midiout.c
                                     ${BMDS_SDK_DIR}/game/shapes.c ${BMDS_SDK_DIR}/game/palette.c
                                     ${BMDS_SDK_DIR}/game/texmap.c
                                     ${BMDS_SDK_DIR}/game/seq.c)
        set_target_properties(bmds_game PROPERTIES OUTPUT_NAME bmdsgame ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_game PUBLIC bmds_headers)
        target_compile_options(bmds_game PRIVATE -O2 -Wall -Wextra -Werror)
        # libbmds3d.a (bmds/3d.h, devplan/3d.md): software 3D on libbmdsgame's canvases.
        add_library(bmds_3d STATIC ${BMDS_SDK_DIR}/3d/math.c ${BMDS_SDK_DIR}/3d/mesh.c
                                   ${BMDS_SDK_DIR}/3d/context.c ${BMDS_SDK_DIR}/3d/wire.c
                                   ${BMDS_SDK_DIR}/3d/solid.c ${BMDS_SDK_DIR}/3d/raster.c
                                   ${BMDS_SDK_DIR}/3d/cores.c ${BMDS_SDK_DIR}/3d/texture.c
                                   ${BMDS_SDK_DIR}/3d/light.c ${BMDS_SDK_DIR}/3d/model.c)
        set_target_properties(bmds_3d PROPERTIES OUTPUT_NAME bmds3d ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_3d PUBLIC bmds_headers)
        target_compile_options(bmds_3d PRIVATE -O2 -Wall -Wextra -Werror)
        # libbmdsgui.a (bmds/gui.h, devplan/guiprograms.md §8): the GUI protocol's client.
        add_library(bmds_gui STATIC ${BMDS_SDK_DIR}/gui/gui.c)
        set_target_properties(bmds_gui PROPERTIES OUTPUT_NAME bmdsgui ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_gui PUBLIC bmds_headers)
        target_compile_options(bmds_gui PRIVATE -Os -Wall -Wextra -Werror)
        # libbmdsbench.a (bmds/benchlib.h, devplan/bench.md §4f): a program's side of `bench`.
        add_library(bmds_benchlib STATIC ${BMDS_SDK_DIR}/bench/bench.c)
        set_target_properties(bmds_benchlib PROPERTIES OUTPUT_NAME bmdsbench ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_benchlib PUBLIC bmds_headers)
        target_compile_options(bmds_benchlib PRIVATE -Os -Wall -Wextra -Werror)
        # libtls.a (tls.h, devplan/network.md §35d): libtls's client calls over the kernel's TLS sockets.
        add_library(bmds_tls STATIC ${BMDS_SDK_DIR}/tls/tls.c)
        set_target_properties(bmds_tls PROPERTIES OUTPUT_NAME tls ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
        target_link_libraries(bmds_tls PUBLIC bmds_headers)
        target_compile_options(bmds_tls PRIVATE -Os -Wall -Wextra -Werror)
        add_custom_target(bmds_libs DEPENDS bmds_libc bmds_crt bmds_game bmds_3d bmds_gui bmds_benchlib bmds_tls)
        # bmds-cc: the same compiler and link for other build systems
        # (CC=.../bmds-cc make), once bmds_libs is built.
        # bmds-c++ the same for C++: g++, the SDK's C++ headers, no exceptions or RTTI.
        set(BMDS_CC_NAME bmds-cc)
        set(BMDS_CC_DRIVER "${CMAKE_C_COMPILER}")
        set(BMDS_CC_LANGUAGE_FLAGS "")
        configure_file(${CMAKE_CURRENT_LIST_DIR}/bmds-cc.in ${CMAKE_BINARY_DIR}/bmds-cc
                       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
                                        WORLD_EXECUTE @ONLY)
        set(BMDS_CC_NAME bmds-c++)
        get_filename_component(_bmds_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
        set(BMDS_CC_DRIVER "${_bmds_cc_bin}/riscv32-esp-elf-g++")
        set(BMDS_CC_LANGUAGE_FLAGS "-std=gnu++20 -isystem ${BMDS_SDK_DIR}/include/c++ -fno-exceptions -fno-rtti")
        configure_file(${CMAKE_CURRENT_LIST_DIR}/bmds-cc.in ${CMAKE_BINARY_DIR}/bmds-c++
                       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
                                        WORLD_EXECUTE @ONLY)
endif()

# The icon notes of program @p name for the PNG files in @p pngs, as an
# assembler file in @p out: each note's descriptor a bmds_icon_note_t and the
# image (.incbin), in a section that isn't loaded.
function(bmds_icon_notes out name)
        set(asm "/* ${name}'s icons (bmds/program.h: BMDS_NOTE_ICON), made by bmds_add_program(). */\n")
        string(APPEND asm "        .section .note.bmds.icon,\"\",@note\n")
        set(label 10)
        foreach(png IN LISTS ARGN)
                get_filename_component(png "${png}" ABSOLUTE)
                file(READ "${png}" header LIMIT 24 HEX)
                string(SUBSTRING "${header}" 0 16 signature)
                if(NOT signature STREQUAL "89504e470d0a1a0a")
                        message(FATAL_ERROR "bmds_add_program(${name}): ${png} isn't a PNG (an LFS pointer? git lfs pull)")
                endif()
                string(SUBSTRING "${header}" 32 8 w)
                string(SUBSTRING "${header}" 40 8 h)
                math(EXPR w "0x${w}")
                math(EXPR h "0x${h}")
                math(EXPR a "${label} + 1")
                math(EXPR b "${label} + 2")
                math(EXPR c "${label} + 3")
                string(APPEND asm "        .balign 4\n"
                                  "        .long 5, ${c}f - ${label}f, 2 /* namesz, descsz, BMDS_NOTE_ICON */\n"
                                  "        .asciz \"BMDS\"\n"
                                  "        .balign 4\n"
                                  "${label}:      .long 16 /* sizeof(bmds_icon_note_t) */\n"
                                  "        .short ${w}, ${h}\n"
                                  "        .long 1, ${b}f - ${a}f /* BMDS_ICON_FORMAT_PNG, its length */\n"
                                  "${a}:      .incbin \"${png}\"\n"
                                  "${b}:\n"
                                  "${c}:\n")
                math(EXPR label "${label} + 10")
        endforeach()
        string(APPEND asm "        .balign 4\n")
        file(GENERATE OUTPUT "${out}" CONTENT "${asm}")
endfunction()

function(bmds_add_program name)
        cmake_parse_arguments(ARG "DISPLAY" "STACK_SIZE" "SOURCES;ICONS" ${ARGN})
        set(icons)
        if(ARG_ICONS)
                set(icons ${CMAKE_CURRENT_BINARY_DIR}/${name}-icons.S)
                set(pngs)
                foreach(png IN LISTS ARG_ICONS)
                        get_filename_component(png "${png}" ABSOLUTE)
                        list(APPEND pngs "${png}")
                endforeach()
                bmds_icon_notes(${icons} ${name} ${pngs})
                set_source_files_properties(${icons} PROPERTIES OBJECT_DEPENDS "${pngs}")
        endif()
        add_executable(${name} ${ARG_SOURCES} ${icons})
        # For bmds_add_package(): its icons are the package's unless it names others.
        set_property(TARGET ${name} PROPERTY BMDS_ICONS "${pngs}")
        target_link_libraries(${name} PRIVATE bmds_headers -Wl,--start-group bmds_game bmds_3d bmds_gui bmds_benchlib bmds_tls bmds_crt bmds_libc
                                      ${BMDS_LIBGCC} -Wl,--end-group)
        target_link_options(${name} PRIVATE
                # Linked at 0 with its relocations kept: the kernel moves it.
                -static -nostdlib -L${BMDS_LIBC_LIB} -u _start -Wl,--emit-relocs
                # Which build it is, for its debug file (devplan/debuginfo.md §3).
                -Wl,--build-id=sha1
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
        if(ARG_DISPLAY)
                target_link_options(${name} PRIVATE -Wl,--defsym=__bmds_program_flags=1) # BMDS_PROGRAM_DISPLAY
        endif()
        # Linked through bmds-link: the bytes the loader patches gathered at the image's end, so more
        # of its pages are shared between instances (devplan/programs.md §14.3).
        set_property(TARGET ${name} PROPERTY RULE_LAUNCH_LINK "${Python3_EXECUTABLE} ${BMDS_SDK_DIR}/tools/bmds-link")
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

# An application folder (devplan/apps.md §3, §14): ${name}.app in the build directory, holding
# PROGRAM (a bmds_add_program() target, stripped, under its own name), DESKTOP as app.desktop,
# and FILES and DIRECTORIES (copied in by their names). Copying the folder into a card's apps/
# installs it. DESKTOP's Exec must name the program (its first word), as the system runs it.
#
#   bmds_add_app(blocks PROGRAM blocks DESKTOP blocks.desktop FILES music.mid DIRECTORIES levels)
function(bmds_add_app name)
        cmake_parse_arguments(ARG "" "PROGRAM;DESKTOP" "FILES;DIRECTORIES" ${ARGN})
        if(NOT ARG_PROGRAM OR NOT ARG_DESKTOP)
                message(FATAL_ERROR "bmds_add_app(${name}): PROGRAM and DESKTOP are needed")
        endif()
        get_filename_component(desktop "${ARG_DESKTOP}" ABSOLUTE)
        # Exec's program, checked now rather than found missing on the board.
        file(STRINGS "${desktop}" exec REGEX "^Exec=")
        list(LENGTH exec execs)
        if(NOT execs EQUAL 1)
                message(FATAL_ERROR "bmds_add_app(${name}): ${desktop} needs one Exec= line")
        endif()
        string(REGEX REPLACE "^Exec=[ \t]*([^ \t]*).*$" "\\1" first "${exec}")
        if(NOT first STREQUAL ARG_PROGRAM)
                message(FATAL_ERROR "bmds_add_app(${name}): Exec runs '${first}', not the program '${ARG_PROGRAM}'")
        endif()
        set(folder ${CMAKE_CURRENT_BINARY_DIR}/${name}.app)
        set(commands COMMAND ${CMAKE_COMMAND} -E rm -rf ${folder}
                     COMMAND ${CMAKE_COMMAND} -E make_directory ${folder}
                     COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${ARG_PROGRAM}.elf
                             ${folder}/${ARG_PROGRAM}
                     COMMAND ${CMAKE_COMMAND} -E copy ${desktop} ${folder}/app.desktop)
        set(depends ${ARG_PROGRAM} ${desktop})
        foreach(f IN LISTS ARG_FILES)
                get_filename_component(f "${f}" ABSOLUTE)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy ${f} ${folder}/)
                list(APPEND depends ${f})
        endforeach()
        foreach(d IN LISTS ARG_DIRECTORIES)
                get_filename_component(d "${d}" ABSOLUTE)
                get_filename_component(base "${d}" NAME)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_directory ${d} ${folder}/${base})
                file(GLOB_RECURSE inside CONFIGURE_DEPENDS "${d}/*")
                list(APPEND depends ${inside})
        endforeach()
        add_custom_command(OUTPUT ${folder}/app.desktop ${commands} DEPENDS ${depends}
                           COMMENT "Making ${name}.app" VERBATIM)
        add_custom_target(${name}.app ALL DEPENDS ${folder}/app.desktop)
        # For bmds_add_package(): where the folder is, its program and its desktop entry.
        set_target_properties(${name}.app PROPERTIES BMDS_APP_FOLDER "${folder}" BMDS_APP_PROGRAM "${ARG_PROGRAM}"
                                                     BMDS_APP_DESKTOP "${desktop}")
endfunction()

include(${CMAKE_CURRENT_LIST_DIR}/BmdsPackage.cmake)
