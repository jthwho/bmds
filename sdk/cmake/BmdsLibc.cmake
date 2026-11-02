# @file      BmdsLibc.cmake
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Builds the C library programs link statically: musl (sdk/libc/musl, see
# sdk/libc/README.md), following its Makefile: every src/*/*.c, src/malloc's
# mallocng, and src/*/riscv32/*.[csS], an architecture file replacing the
# generic one of the same name. The public headers are assembled in the
# build tree at configure time (musl's include/, its generic and riscv32
# bits, and the two it generates). Defines:
#   bmds_libc             the static library (libc.a)
#   BMDS_LIBC_INCLUDE     its public headers
#   BMDS_LIBC_LIB         the library directory: libc.a, and empty libm.a,
#                         libpthread.a... (all of it is in libc.a, as in musl),
#                         searched before the toolchain's, which has newlib's
#   BMDS_LIBGCC           libgcc.a for the BMDS architecture flags

if(TARGET bmds_libc)
        return()
endif()

get_filename_component(_bmds_musl "${CMAKE_CURRENT_LIST_DIR}/../libc/musl" ABSOLUTE)
set(_bmds_gen "${CMAKE_BINARY_DIR}/bmds-libc")
set(BMDS_LIBC_INCLUDE "${_bmds_gen}/include")
set(BMDS_LIBC_LIB "${_bmds_gen}/lib")

# --- Headers -----------------------------------------------------------------

file(READ "${_bmds_musl}/VERSION" _bmds_musl_version)
string(STRIP "${_bmds_musl_version}" _bmds_musl_version)

# tools/mkalltypes.sed, in CMake: TYPEDEF/STRUCT/UNION lines become guarded
# definitions.
function(_bmds_alltypes out)
        set(text "")
        foreach(in "${_bmds_musl}/arch/riscv32/bits/alltypes.h.in" "${_bmds_musl}/include/alltypes.h.in")
                file(STRINGS "${in}" lines)
                foreach(line IN LISTS lines)
                        if(line MATCHES "^TYPEDEF (.*) ([^ ]*);$")
                                set(n "${CMAKE_MATCH_2}")
                                string(APPEND text "#if defined(__NEED_${n}) && !defined(__DEFINED_${n})\n"
                                                   "typedef ${CMAKE_MATCH_1} ${n};\n#define __DEFINED_${n}\n#endif\n\n")
                        elseif(line MATCHES "^(STRUCT|UNION) *([^ ]*) (.*);$")
                                string(TOLOWER "${CMAKE_MATCH_1}" kind)
                                set(n "${CMAKE_MATCH_2}")
                                string(APPEND text "#if defined(__NEED_${kind}_${n}) && !defined(__DEFINED_${kind}_${n})\n"
                                                   "${kind} ${n} ${CMAKE_MATCH_3};\n#define __DEFINED_${kind}_${n}\n#endif\n\n")
                        else()
                                string(APPEND text "${line}\n")
                        endif()
                endforeach()
        endforeach()
        file(WRITE "${out}.tmp" "${text}")
        file(COPY_FILE "${out}.tmp" "${out}" ONLY_IF_DIFFERENT)
        file(REMOVE "${out}.tmp")
endfunction()

# Copied at configure time, so a change to any of them configures again (else a build tree keeps
# the headers it was made with).
file(GLOB_RECURSE _bmds_headers CONFIGURE_DEPENDS "${_bmds_musl}/include/*" "${_bmds_musl}/arch/generic/bits/*.h"
     "${_bmds_musl}/arch/riscv32/bits/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_bmds_headers})
file(COPY "${_bmds_musl}/include/" DESTINATION "${BMDS_LIBC_INCLUDE}" PATTERN "*.in" EXCLUDE)
file(GLOB _bmds_bits "${_bmds_musl}/arch/generic/bits/*.h")
file(COPY ${_bmds_bits} DESTINATION "${BMDS_LIBC_INCLUDE}/bits")
file(GLOB _bmds_bits "${_bmds_musl}/arch/riscv32/bits/*.h")
file(COPY ${_bmds_bits} DESTINATION "${BMDS_LIBC_INCLUDE}/bits")
_bmds_alltypes("${BMDS_LIBC_INCLUDE}/bits/alltypes.h")
# bits/syscall.h: the __NR_ names, and the same as SYS_.
file(READ "${_bmds_musl}/arch/riscv32/bits/syscall.h.in" _bmds_sys)
string(REGEX MATCHALL "#define __NR_[^\n]*" _bmds_nr "${_bmds_sys}")
list(TRANSFORM _bmds_nr REPLACE "__NR_" "SYS_")
list(JOIN _bmds_nr "\n" _bmds_nr)
file(CONFIGURE OUTPUT "${BMDS_LIBC_INCLUDE}/bits/syscall.h" CONTENT "${_bmds_sys}${_bmds_nr}\n" @ONLY)
file(CONFIGURE OUTPUT "${_bmds_gen}/internal/version.h" CONTENT "#define VERSION \"${_bmds_musl_version}\"\n" @ONLY)

# --- Sources -----------------------------------------------------------------

file(GLOB _bmds_dirs LIST_DIRECTORIES true "${_bmds_musl}/src/*")
list(APPEND _bmds_dirs "${_bmds_musl}/src/malloc/mallocng")
set(_bmds_srcs "")
foreach(dir IN LISTS _bmds_dirs)
        if(NOT IS_DIRECTORY "${dir}")
                continue()
        endif()
        file(GLOB base "${dir}/*.c")
        file(GLOB arch "${dir}/riscv32/*.c" "${dir}/riscv32/*.s" "${dir}/riscv32/*.S")
        foreach(a IN LISTS arch)
                get_filename_component(stem "${a}" NAME_WE)
                list(REMOVE_ITEM base "${dir}/${stem}.c")
        endforeach()
        list(APPEND _bmds_srcs ${base} ${arch})
endforeach()

# The SDK's vfork.S has vfork and __clone (musl's generic ones are fork and
# ENOSYS); its linux.c has __unmapself (musl's generic one switches to a
# shared stack to unmap its own: BMDS's thread_exit frees the mapping).
list(REMOVE_ITEM _bmds_srcs "${_bmds_musl}/src/process/vfork.c" "${_bmds_musl}/src/thread/clone.c"
     "${_bmds_musl}/src/thread/__unmapself.c")

add_library(bmds_libc STATIC ${_bmds_srcs})
set_target_properties(bmds_libc PROPERTIES OUTPUT_NAME c ARCHIVE_OUTPUT_DIRECTORY "${BMDS_LIBC_LIB}")
# What other libcs keep apart is all in libc.a: -lm and the like must find
# these empty archives, not the toolchain's newlib ones.
foreach(stub m pthread rt dl util xnet resolv crypt)
        if(NOT EXISTS "${BMDS_LIBC_LIB}/lib${stub}.a")
                file(WRITE "${BMDS_LIBC_LIB}/lib${stub}.a" "!<arch>\n")
        endif()
endforeach()
target_include_directories(bmds_libc PRIVATE
        "${_bmds_musl}/arch/riscv32" "${_bmds_musl}/arch/generic" "${_bmds_gen}/internal"
        "${_bmds_musl}/src/include" "${_bmds_musl}/src/internal" "${BMDS_LIBC_INCLUDE}" "${_bmds_musl}/include")
# musl's own flags (configure's CFLAGS_C99FSE, CFLAGS_AUTO, CPPFLAGS); -Os
# whatever the build type, after its flags.
target_compile_options(bmds_libc PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-std=c99 -ffreestanding -fexcess-precision=standard -frounding-math
                -fno-strict-aliasing -fomit-frame-pointer -Wno-pointer-to-int-cast>
        -nostdinc -fno-unwind-tables -fno-asynchronous-unwind-tables -Wa,--noexecstack -Os
        # musl decides about NDEBUG itself (its regex code defines it).
        -UNDEBUG)
target_compile_definitions(bmds_libc PRIVATE _XOPEN_SOURCE=700)
# The memory functions must not be turned into calls to themselves.
file(GLOB _bmds_memops "${_bmds_musl}/src/string/mem*.c")
set_source_files_properties(${_bmds_memops} PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
# GCC 15 can't see that getcwd() only returns its local buffer after
# copying it out (musl 1.2.5).
set_source_files_properties("${_bmds_musl}/src/unistd/getcwd.c" PROPERTIES COMPILE_OPTIONS "-Wno-return-local-addr")

# --- libgcc --------------------------------------------------------------------

if(NOT BMDS_LIBGCC)
        separate_arguments(_bmds_arch UNIX_COMMAND "${BMDS_ARCH_FLAGS}")
        execute_process(COMMAND ${CMAKE_C_COMPILER} ${_bmds_arch} -print-libgcc-file-name
                        OUTPUT_VARIABLE _bmds_libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(BMDS_LIBGCC "${_bmds_libgcc}" CACHE FILEPATH "libgcc.a for BMDS programs")
endif()
