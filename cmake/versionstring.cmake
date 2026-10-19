# The firmware's version text (devplan/update.md §10): bmds/version.h's BMDS_VERSION_* (the number
# an update compares), then "+" and the commit ("-dirty" with uncommitted changes):
# "0.1.0+72cba49a". Used at configure time (version.cmake) and at every build (version-text.cmake).

include_guard(GLOBAL)

# Sets @p out to the version text of the source tree at @p root; fails past the 31 characters the
# app description holds.
function(bmds_version_string out root)
        set(version_h ${root}/components/bmds_core/include/bmds/version.h)
        file(STRINGS ${version_h} lines REGEX "^#define BMDS_VERSION_(MAJOR|MINOR|PATCH) ")
        foreach(part MAJOR MINOR PATCH)
                if(NOT lines MATCHES "BMDS_VERSION_${part} ([0-9]+)")
                        message(FATAL_ERROR "${version_h}: no BMDS_VERSION_${part}")
                endif()
                set(${part} ${CMAKE_MATCH_1})
        endforeach()
        set(text "${MAJOR}.${MINOR}.${PATCH}")
        find_package(Git QUIET)
        if(GIT_FOUND)
                execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD WORKING_DIRECTORY ${root}
                                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
                                RESULT_VARIABLE result ERROR_QUIET)
                if(result EQUAL 0)
                        execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD WORKING_DIRECTORY ${root}
                                        RESULT_VARIABLE dirty ERROR_QUIET)
                        if(NOT dirty EQUAL 0)
                                string(APPEND commit "-dirty")
                        endif()
                        string(APPEND text "+${commit}")
                endif()
        endif()
        string(LENGTH "${text}" length)
        if(length GREATER 31)
                message(FATAL_ERROR "version ${text}: the app description holds 31 characters")
        endif()
        set(${out} "${text}" PARENT_SCOPE)
endfunction()
