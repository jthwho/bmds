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
                $<$<COMPILE_LANGUAGE:C>:-std=gnu23>
                -Wall -Wextra -Werror -Wshadow)
endfunction()
