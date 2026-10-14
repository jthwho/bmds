# The firmware's version (devplan/update.md §10): bmds/version.h's BMDS_VERSION_* (the number an
# update compares), then "+" and the commit ("-dirty" with uncommitted changes), as PROJECT_VER:
# "0.1.0+72cba49a". Included by the IDF projects before project(); read at configure time.

include_guard(GLOBAL)

set(bmds_version_h ${CMAKE_CURRENT_LIST_DIR}/../components/bmds_core/include/bmds/version.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${bmds_version_h})
file(STRINGS ${bmds_version_h} bmds_version_lines
     REGEX "^#define BMDS_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(part MAJOR MINOR PATCH)
        if(NOT bmds_version_lines MATCHES "BMDS_VERSION_${part} ([0-9]+)")
                message(FATAL_ERROR "${bmds_version_h}: no BMDS_VERSION_${part}")
        endif()
        set(BMDS_VERSION_${part} ${CMAKE_MATCH_1})
endforeach()
set(PROJECT_VER "${BMDS_VERSION_MAJOR}.${BMDS_VERSION_MINOR}.${BMDS_VERSION_PATCH}")
find_package(Git QUIET)
if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
                        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                        OUTPUT_VARIABLE bmds_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE bmds_git_result ERROR_QUIET)
        if(bmds_git_result EQUAL 0)
                execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
                                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                                RESULT_VARIABLE bmds_dirty ERROR_QUIET)
                if(NOT bmds_dirty EQUAL 0)
                        string(APPEND bmds_commit "-dirty")
                endif()
                string(APPEND PROJECT_VER "+${bmds_commit}")
        endif()
endif()
string(LENGTH "${PROJECT_VER}" bmds_version_length)
if(bmds_version_length GREATER 31)
        message(FATAL_ERROR "PROJECT_VER ${PROJECT_VER}: the app description holds 31 characters")
endif()
