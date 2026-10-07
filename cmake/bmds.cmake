# Shared build settings for BMDS components.
#
# Included from each BMDS component's CMakeLists.txt (both in the ESP-IDF
# build and in the host test build), so it must not depend on anything
# defined by the top-level project.

include_guard(GLOBAL)

# Applies the BMDS language standard and warning policy to a target.
#
# ESP-IDF builds everything with its own (newer) -std; BMDS code is written
# to C++20, so the BMDS flag is added after IDF's and wins.
function(bmds_target_options target)
        get_target_property(target_type ${target} TYPE)
        if(target_type STREQUAL "INTERFACE_LIBRARY")
                return()
        endif()
        target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20 -fno-exceptions -fno-rtti -Wnon-virtual-dtor>
                $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
                -Wall -Wextra -Werror -Wshadow)
endfunction()

# A built-in program's icons (devplan/gui.md §4j): the PNGs in ARGN as a header
# named HEADER (in the target's binary directory, on its include path) defining
# the ProgramIcon array VAR for BMDS_PROGRAM_ICONS.
function(bmds_embed_icons target header var)
        set(script ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/embedicons.py)
        if(COMMAND idf_build_get_property)
                idf_build_get_property(python PYTHON)
        else()
                find_package(Python3 REQUIRED COMPONENTS Interpreter)
                set(python ${Python3_EXECUTABLE})
        endif()
        set(out ${CMAKE_CURRENT_BINARY_DIR}/${header})
        add_custom_command(OUTPUT ${out}
                           COMMAND ${python} ${script} ${out} ${var} ${ARGN}
                           DEPENDS ${script} ${ARGN}
                           VERBATIM)
        target_sources(${target} PRIVATE ${out})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
