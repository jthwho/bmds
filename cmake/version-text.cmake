# Run at every build (cmake -DROOT=<source tree> -DOUT=<header> -P version-text.cmake): writes the
# header that gives ESP-IDF's app description this build's version (cmake/version.cmake), and
# rewrites it only when the version changed, so an unchanged build compiles nothing again.

include(${CMAKE_CURRENT_LIST_DIR}/versionstring.cmake)
bmds_version_string(text ${ROOT})
set(content "/* Generated at every build by cmake/version-text.cmake: the firmware's version. */
#undef PROJECT_VER
#define PROJECT_VER \"${text}\"
")
set(old "")
if(EXISTS ${OUT})
        file(READ ${OUT} old)
endif()
if(NOT old STREQUAL content)
        file(WRITE ${OUT} "${content}")
endif()
