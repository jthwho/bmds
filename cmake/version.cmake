# The firmware's version (devplan/update.md §10, cmake/versionstring.cmake): "0.1.0+72cba49a".
# Included by the IDF projects before project(), which takes it as PROJECT_VER at configure time;
# after project(), bmds_version_every_build() makes the app description take it at every build,
# so a commit or a change to the tree shows without configuring again.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/versionstring.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/../components/bmds_core/include/bmds/version.h)
bmds_version_string(PROJECT_VER ${CMAKE_CURRENT_LIST_DIR}/..)

# esp_app_desc.c (esp_app_format) gets PROJECT_VER as a definition made at configure time; a
# header regenerated before every build (rewritten only when the version changes) is included
# into it first and replaces the definition.
function(bmds_version_every_build)
        set(header ${CMAKE_BINARY_DIR}/bmds_version_text.h)
        add_custom_target(bmds_version_text
                          COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..
                                  -DOUT=${header} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/version-text.cmake
                          BYPRODUCTS ${header}
                          COMMENT "Checking the version"
                          VERBATIM)
        idf_component_get_property(app_format esp_app_format COMPONENT_LIB)
        add_dependencies(${app_format} bmds_version_text)
        target_compile_options(${app_format} PRIVATE -include ${header})
endfunction()
