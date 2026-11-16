# BmdsPackage.cmake: packages (.bmpk) and a repository (index.bmpi) from SDK programs
# (devplan/thirdparty.md §6, devplan/packages.md). Included by BmdsProgram.cmake.
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
#   bmds_add_package(minesweeper VERSION 1.0.0
#           [TITLE "Minesweeper"] [SUMMARY "One line"] [DESCRIPTION_FILE DESCRIPTION]
#           [CATEGORY Game] [LICENSE MIT] [HOMEPAGE https://...] [DEPENDS "lib (>= 2)" ...]
#           [APPS minesweeper ...]      # bmds_add_app() names: into apps/NAME.app
#           [PROGRAMS tool ...]         # bmds_add_program() targets: into bin/NAME
#           [FILES share/x/a.txt=src/a.txt ...]   # anything else, by its place in the package
#           [ICONS a-16.png a-32.png] [ABI N])
#   bmds_add_repository(packages PACKAGES minesweeper ... [BASE_URL URL] [LABEL TEXT])
#
# bmds_add_package() stages the package's files under the build directory and runs mkpkg make
# into packages/NAME_VERSION.bmpk (the target NAME.bmpk, built by default). The title and
# category come from the one application's app.desktop (Name, Categories) unless given; the icons
# from its program's ICONS. ABI, the least BMDS_ABI_VERSION it needs, defaults to the SDK's.
#
# bmds_add_repository() makes a folder (the target NAME, built by default) holding the packages
# named, their index.bmpi and key.pub: what a web server or a GitHub release serves, and what
# `pkg source add URL` reads. Its files are named relatively unless BASE_URL is given.
#
# Signing: the cache variable BMDS_KEY names the private key (made once with `mkpkg keygen`,
# kept out of every repository; keys/README.md); its public half is BMDS_KEY with .pub for its
# extension unless BMDS_KEY_PUB says. Unset, the dev key signs, and the build says so: fine for
# trying, not for publishing.
#
# mkpkg needs Python 3 with the `cryptography` module (ESP-IDF's Python has it; else
# `pip install cryptography`).

set(BMDS_KEY "" CACHE FILEPATH "The private key packages and indexes are signed with (empty: the dev key)")
set(BMDS_KEY_PUB "" CACHE FILEPATH "Its public half (empty: BMDS_KEY with .pub for its extension)")

get_filename_component(BMDS_TOOLS_DIR "${BMDS_SDK_DIR}/../tools" ABSOLUTE)
set(BMDS_MKPKG "${BMDS_TOOLS_DIR}/mkpkg")

# Which key signs, and its public half: BMDS_KEY_ARGS (for mkpkg) and BMDS_KEY_PUBLIC.
function(_bmds_package_key)
        if(BMDS_KEY)
                get_filename_component(key "${BMDS_KEY}" ABSOLUTE)
                if(NOT EXISTS "${key}")
                        message(FATAL_ERROR "BMDS_KEY: ${key} doesn't exist (mkpkg keygen makes a key)")
                endif()
                set(pub "${BMDS_KEY_PUB}")
                if(NOT pub)
                        get_filename_component(dir "${key}" DIRECTORY)
                        get_filename_component(stem "${key}" NAME_WLE)
                        set(pub "${dir}/${stem}.pub")
                endif()
                set(BMDS_KEY_ARGS --key "${key}" PARENT_SCOPE)
                set(BMDS_KEY_FILE "${key}" PARENT_SCOPE)
        else()
                set(pub "${BMDS_SDK_DIR}/../keys/dev.pub")
                set(BMDS_KEY_ARGS --key "${BMDS_SDK_DIR}/../keys/dev.key" PARENT_SCOPE)
                set(BMDS_KEY_FILE "${BMDS_SDK_DIR}/../keys/dev.key" PARENT_SCOPE)
                get_property(said GLOBAL PROPERTY _BMDS_DEV_KEY_SAID)
                if(NOT said)
                        message(WARNING "BMDS_KEY isn't set: packages are signed with the dev key, which anyone "
                                        "has. Fine for trying; to publish, make a key (mkpkg keygen) and set "
                                        "BMDS_KEY (keys/README.md).")
                        set_property(GLOBAL PROPERTY _BMDS_DEV_KEY_SAID TRUE)
                endif()
        endif()
        get_filename_component(pub "${pub}" ABSOLUTE)
        if(NOT EXISTS "${pub}")
                message(FATAL_ERROR "${pub}: the public half of the signing key isn't there (BMDS_KEY_PUB)")
        endif()
        set(BMDS_KEY_PUBLIC "${pub}" PARENT_SCOPE)
endfunction()

# mkpkg's Python, checked once: it signs with `cryptography`.
function(_bmds_check_mkpkg)
        get_property(checked GLOBAL PROPERTY _BMDS_MKPKG_CHECKED)
        if(checked)
                return()
        endif()
        if(NOT EXISTS "${BMDS_MKPKG}")
                message(FATAL_ERROR "mkpkg isn't at ${BMDS_MKPKG}: the SDK's tools/ folder is beside sdk/")
        endif()
        execute_process(COMMAND ${Python3_EXECUTABLE} -c "import cryptography" RESULT_VARIABLE missing
                        OUTPUT_QUIET ERROR_QUIET)
        if(missing)
                message(FATAL_ERROR "${Python3_EXECUTABLE} has no `cryptography` module, which mkpkg signs "
                                    "with: pip install cryptography (or use ESP-IDF's Python)")
        endif()
        set_property(GLOBAL PROPERTY _BMDS_MKPKG_CHECKED TRUE)
endfunction()

# A key=value line of a desktop file, or empty; a list's `;`s as `,`s.
function(_bmds_desktop_value out desktop key)
        file(STRINGS "${desktop}" lines REGEX "^${key}=")
        set(value "")
        if(lines)
                list(GET lines 0 line)
                string(REGEX REPLACE "^${key}=" "" value "${line}")
                string(REPLACE "\\;" "," value "${value}") # file(STRINGS) writes a ';' as "\;"
                string(REPLACE ";" "," value "${value}")
        endif()
        set(${out} "${value}" PARENT_SCOPE)
endfunction()

# The freedesktop.org main categories the launcher sorts by (devplan/apps.md).
set(_BMDS_MAIN_CATEGORIES AudioVideo Audio Video Development Education Game Graphics Network Office Science
                          Settings System Utility)

function(bmds_add_package name)
        cmake_parse_arguments(ARG "" "VERSION;TITLE;SUMMARY;DESCRIPTION_FILE;CATEGORY;LICENSE;HOMEPAGE;ABI"
                              "DEPENDS;APPS;PROGRAMS;FILES;ICONS" ${ARGN})
        if(NOT ARG_VERSION)
                message(FATAL_ERROR "bmds_add_package(${name}): VERSION is needed")
        endif()
        _bmds_check_mkpkg()
        _bmds_package_key()
        set(stage "${CMAKE_CURRENT_BINARY_DIR}/${name}.pkgroot")
        set(out_dir "${CMAKE_BINARY_DIR}/packages")
        set(out "${out_dir}/${name}_${ARG_VERSION}.bmpk")
        set(commands COMMAND ${CMAKE_COMMAND} -E rm -rf "${stage}" COMMAND ${CMAKE_COMMAND} -E make_directory "${stage}")
        set(depends "${BMDS_KEY_FILE}" "${BMDS_MKPKG}")
        set(targets)
        set(icons ${ARG_ICONS})
        set(title "${ARG_TITLE}")
        set(category "${ARG_CATEGORY}")
        list(LENGTH ARG_APPS app_count)

        foreach(app IN LISTS ARG_APPS)
                if(NOT TARGET ${app}.app)
                        message(FATAL_ERROR "bmds_add_package(${name}): no application ${app} (bmds_add_app first)")
                endif()
                get_target_property(folder ${app}.app BMDS_APP_FOLDER)
                get_target_property(program ${app}.app BMDS_APP_PROGRAM)
                get_target_property(desktop ${app}.app BMDS_APP_DESKTOP)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${folder}" "${stage}/apps/${app}.app")
                list(APPEND depends "${folder}/app.desktop" ${program})
                list(APPEND targets ${app}.app)
                if(app_count EQUAL 1)
                        if(NOT icons)
                                get_target_property(icons ${program} BMDS_ICONS)
                        endif()
                        if(NOT title)
                                _bmds_desktop_value(title "${desktop}" Name)
                        endif()
                        if(NOT category)
                                _bmds_desktop_value(categories "${desktop}" Categories)
                                foreach(main IN LISTS _BMDS_MAIN_CATEGORIES)
                                        if(NOT category AND ",${categories}," MATCHES ",${main},")
                                                set(category ${main})
                                        endif()
                                endforeach()
                        endif()
                endif()
        endforeach()
        foreach(program IN LISTS ARG_PROGRAMS)
                if(NOT TARGET ${program})
                        message(FATAL_ERROR "bmds_add_package(${name}): no program ${program} (bmds_add_program)")
                endif()
                get_target_property(bin ${program} BINARY_DIR)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E make_directory "${stage}/bin"
                                     COMMAND ${CMAKE_COMMAND} -E copy "${bin}/${program}.elf" "${stage}/bin/${program}")
                list(APPEND depends ${program})
                list(APPEND targets ${program})
        endforeach()
        foreach(file IN LISTS ARG_FILES)
                if(NOT file MATCHES "^([^=]+)=(.+)$")
                        message(FATAL_ERROR "bmds_add_package(${name}): FILES are PLACE=SOURCE: ${file}")
                endif()
                set(place "${CMAKE_MATCH_1}")
                get_filename_component(source "${CMAKE_MATCH_2}" ABSOLUTE)
                get_filename_component(dir "${stage}/${place}" DIRECTORY)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
                                     COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${stage}/${place}")
                list(APPEND depends "${source}")
        endforeach()

        set(make ${Python3_EXECUTABLE} "${BMDS_MKPKG}" make "${stage}" --name ${name} --version ${ARG_VERSION}
                 --arch esp32p4 ${BMDS_KEY_ARGS} -o "${out}")
        foreach(field TITLE:title SUMMARY:ARG_SUMMARY CATEGORY:category LICENSE:ARG_LICENSE HOMEPAGE:ARG_HOMEPAGE
                      ABI:ARG_ABI)
                string(REPLACE ":" ";" pair "${field}")
                list(GET pair 0 option)
                list(GET pair 1 variable)
                if(NOT "${${variable}}" STREQUAL "")
                        string(TOLOWER "${option}" option)
                        list(APPEND make --${option} "${${variable}}")
                endif()
        endforeach()
        if(ARG_DEPENDS)
                list(JOIN ARG_DEPENDS ", " depends_text)
                list(APPEND make --depends "${depends_text}")
        endif()
        if(ARG_DESCRIPTION_FILE)
                get_filename_component(description "${ARG_DESCRIPTION_FILE}" ABSOLUTE)
                list(APPEND make --description-file "${description}")
                list(APPEND depends "${description}")
        endif()
        foreach(icon IN LISTS icons)
                get_filename_component(icon "${icon}" ABSOLUTE)
                list(APPEND make --icon "${icon}")
                list(APPEND depends "${icon}")
        endforeach()

        add_custom_command(OUTPUT "${out}" ${commands}
                           COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
                           COMMAND ${make}
                           DEPENDS ${depends} COMMENT "Packaging ${name}_${ARG_VERSION}.bmpk" VERBATIM)
        add_custom_target(${name}.bmpk ALL DEPENDS "${out}")
        if(targets)
                add_dependencies(${name}.bmpk ${targets})
        endif()
        set_target_properties(${name}.bmpk PROPERTIES BMDS_PACKAGE_FILE "${out}")
endfunction()

function(bmds_add_repository name)
        cmake_parse_arguments(ARG "" "BASE_URL;LABEL" "PACKAGES" ${ARGN})
        _bmds_check_mkpkg()
        _bmds_package_key()
        set(dir "${CMAKE_BINARY_DIR}/${name}")
        set(commands COMMAND ${CMAKE_COMMAND} -E rm -rf "${dir}" COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}")
        set(depends "${BMDS_KEY_FILE}" "${BMDS_KEY_PUBLIC}")
        set(targets)
        foreach(package IN LISTS ARG_PACKAGES)
                if(NOT TARGET ${package}.bmpk)
                        message(FATAL_ERROR "bmds_add_repository(${name}): no package ${package} (bmds_add_package)")
                endif()
                get_target_property(file ${package}.bmpk BMDS_PACKAGE_FILE)
                list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy "${file}" "${dir}/")
                list(APPEND depends "${file}")
                list(APPEND targets ${package}.bmpk)
        endforeach()
        set(index ${Python3_EXECUTABLE} "${BMDS_MKPKG}" index "${dir}" ${BMDS_KEY_ARGS})
        if(ARG_BASE_URL)
                list(APPEND index --base-url "${ARG_BASE_URL}")
        endif()
        if(ARG_LABEL)
                list(APPEND index --label "${ARG_LABEL}")
        endif()
        add_custom_command(OUTPUT "${dir}/index.bmpi" ${commands}
                           COMMAND ${index}
                           COMMAND ${CMAKE_COMMAND} -E copy "${BMDS_KEY_PUBLIC}" "${dir}/key.pub"
                           # Checked as a board will: the index's signature, against its own key.
                           COMMAND ${Python3_EXECUTABLE} "${BMDS_MKPKG}" info "${dir}/index.bmpi" --pub
                                   "${BMDS_KEY_PUBLIC}"
                           DEPENDS ${depends} COMMENT "Indexing the repository ${name}/" VERBATIM)
        add_custom_target(${name} ALL DEPENDS "${dir}/index.bmpi")
        if(targets)
                add_dependencies(${name} ${targets})
        endif()
endfunction()
