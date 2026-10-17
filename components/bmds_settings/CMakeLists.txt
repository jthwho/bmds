# bmds_settings — /bin/settings, the control panel (devplan/controlpanel.md): every system setting
# (sysconf) in grouped pages, laid out like the Mac's System Settings; an ordinary GUI program on
# the public toolkit and the registry's public calls, built into the firmware only to have it at
# hand. Nothing in the OS refers to it.
#
# Its pure part (the pages' tables: where each setting goes, its label and words) is also built as
# a plain CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

set(BMDS_SETTINGS_PURE_SRCS
        src/settingstable.cpp
        src/themes.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_SETTINGS_PURE_SRCS}
                                    src/pagestatus.cpp
                                    src/settingsapp.cpp
                                    src/settingsdialogs.cpp
                                    src/previews.cpp
                                    src/settingspage.cpp
                                    src/settingswindow.cpp
                               PRIV_REQUIRES bmds_audio bmds_core bmds_fs bmds_gui bmds_input bmds_kernel bmds_loader bmds_net bmds_sysconf
                                             bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})

        # Its icons, built in (devplan/gui.md §4j): the flat-color theme's settings, as the bmds
        # theme's preferences-system.
        set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
        bmds_embed_icons(${COMPONENT_LIB} settingsicons.h SettingsIcons ${BMDS_ICONS}/16x16/settings.png
                         ${BMDS_ICONS}/32x32/settings.png)
else()
        add_library(bmds_settings STATIC ${BMDS_SETTINGS_PURE_SRCS})
        target_include_directories(bmds_settings PUBLIC src)
        target_link_libraries(bmds_settings PUBLIC bmds_core)
        bmds_target_options(bmds_settings)
endif()
