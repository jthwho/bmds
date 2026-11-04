# BMDS host unit tests (plain CMake, no ESP-IDF).
#
#   cmake -S tests/host -B build-host
#   cd build-host && build && ctest --output-on-failure

cmake_minimum_required(VERSION 3.22)
project(bmds_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(BMDS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${BMDS_ROOT}/components/bmds_core ${CMAKE_BINARY_DIR}/bmds_core)
add_subdirectory(${BMDS_ROOT}/components/bmds_bench ${CMAKE_BINARY_DIR}/bmds_bench)
add_subdirectory(${BMDS_ROOT}/components/bmds_kernel ${CMAKE_BINARY_DIR}/bmds_kernel)
add_subdirectory(${BMDS_ROOT}/components/bmds_console ${CMAKE_BINARY_DIR}/bmds_console)
add_subdirectory(${BMDS_ROOT}/components/bmds_shell ${CMAKE_BINARY_DIR}/bmds_shell)
add_subdirectory(${BMDS_ROOT}/components/bmds_pkg ${CMAKE_BINARY_DIR}/bmds_pkg)
add_subdirectory(${BMDS_ROOT}/components/bmds_fs ${CMAKE_BINARY_DIR}/bmds_fs)
add_subdirectory(${BMDS_ROOT}/components/bmds_files ${CMAKE_BINARY_DIR}/bmds_files)
add_subdirectory(${BMDS_ROOT}/components/bmds_loader ${CMAKE_BINARY_DIR}/bmds_loader)
add_subdirectory(${BMDS_ROOT}/components/bmds_drivers ${CMAKE_BINARY_DIR}/bmds_drivers)
add_subdirectory(${BMDS_ROOT}/components/bmds_audio ${CMAKE_BINARY_DIR}/bmds_audio)
add_subdirectory(${BMDS_ROOT}/components/bmds_midi ${CMAKE_BINARY_DIR}/bmds_midi)
add_subdirectory(${BMDS_ROOT}/components/bmds_synth ${CMAKE_BINARY_DIR}/bmds_synth)
add_subdirectory(${BMDS_ROOT}/components/bmds_fonts ${CMAKE_BINARY_DIR}/bmds_fonts)
add_subdirectory(${BMDS_ROOT}/components/bmds_speech ${CMAKE_BINARY_DIR}/bmds_speech)
add_subdirectory(${BMDS_ROOT}/components/bmds_mplayer ${CMAKE_BINARY_DIR}/bmds_mplayer)
add_subdirectory(${BMDS_ROOT}/components/bmds_sysmon ${CMAKE_BINARY_DIR}/bmds_sysmon)
add_subdirectory(${BMDS_ROOT}/components/bmds_settings ${CMAKE_BINARY_DIR}/bmds_settings)
add_subdirectory(${BMDS_ROOT}/components/bmds_imageview ${CMAKE_BINARY_DIR}/bmds_imageview)
add_subdirectory(${BMDS_ROOT}/components/bmds_panel ${CMAKE_BINARY_DIR}/bmds_panel)
add_subdirectory(${BMDS_ROOT}/components/bmds_input ${CMAKE_BINARY_DIR}/bmds_input)
add_subdirectory(${BMDS_ROOT}/components/bmds_ui ${CMAKE_BINARY_DIR}/bmds_ui)
add_subdirectory(${BMDS_ROOT}/components/bmds_gui ${CMAKE_BINARY_DIR}/bmds_gui)
add_subdirectory(${BMDS_ROOT}/components/bmds_clipboard ${CMAKE_BINARY_DIR}/bmds_clipboard)
add_subdirectory(${BMDS_ROOT}/components/bmds_sysconf ${CMAKE_BINARY_DIR}/bmds_sysconf)
add_subdirectory(${BMDS_ROOT}/components/bmds_net ${CMAKE_BINARY_DIR}/bmds_net)

add_executable(bmds_host_tests
        main.cpp
        hostmemory.cpp
        arena.cpp
        audio.cpp
        bench.cpp
        ${BMDS_ROOT}/sdk/bench/bench.c
        blockdevice.cpp
        bytering.cpp
        intervaltimers.cpp
        clipboard.cpp
        decoder.cpp
        dragdrop.cpp
        edid.cpp
        futex.cpp
        gui.cpp
        settings.cpp
        imagecache.cpp
        memtest.cpp
        keptrecord.cpp
        pagewindow.cpp
        error.cpp
        ethernet.cpp
        fdtable.cpp
        files.cpp
        filetypes.cpp
        folders.cpp
        input.cpp
        interactive.cpp
        interrupt.cpp
        json.cpp
        language.cpp
        launch.cpp
        lineeditor.cpp
        lfsvfs.cpp
        tmpfs.cpp
        netvolume.cpp
        servedfs.cpp
        sshfs.cpp
        netmounts.cpp
        sshfs_posix.c
        ${BMDS_ROOT}/programs/sshfs/sshfs.c
        ${BMDS_ROOT}/programs/sftp-server/sftp.c
        list.cpp
        littlefs.cpp
        lwip.cpp
        lwipwire.cpp
        socket.cpp
        loader.cpp
        blockwriter.cpp
        memorystream.cpp
        oss.cpp
        ${BMDS_ROOT}/sdk/crt/oss.c
        fakefonts.cpp
        fontlibrary.cpp
        fonts.cpp
        game.cpp
        ${BMDS_ROOT}/sdk/game/canvas.c
        ${BMDS_ROOT}/sdk/game/shapes.c
        ${BMDS_ROOT}/sdk/game/text.c
        ${BMDS_ROOT}/sdk/gui/gui.c
        ${BMDS_ROOT}/sdk/game/convert.c
        ${BMDS_ROOT}/sdk/game/mix.c
        ${BMDS_ROOT}/sdk/game/smf.c
        blocks.cpp
        ${BMDS_ROOT}/programs/blocks/rules.c
        solitaire.cpp
        ${BMDS_ROOT}/programs/luckyduck/solitaire_rules.c
        luckyduck_blackjack.cpp
        ${BMDS_ROOT}/programs/luckyduck/blackjack_rules.c
        luckyduck_dice.cpp
        ${BMDS_ROOT}/programs/luckyduck/dice_rules.c
        luckyduck_video.cpp
        ${BMDS_ROOT}/programs/luckyduck/video_rules.c
        luckyduck_poker.cpp
        ${BMDS_ROOT}/programs/luckyduck/poker_rules.c
        ${BMDS_ROOT}/programs/luckyduck/poker_bot.c
        ${BMDS_ROOT}/programs/luckyduck/hand.c
        ${BMDS_ROOT}/programs/luckyduck/deck.c
        memspace.cpp
        midi.cpp
        rtpmidi.cpp
        mplayer.cpp
        sysmon.cpp
        cpumeter.cpp
        wallpaper.cpp
        netclassifier.cpp
        icmpratelimit.cpp
        realtime.cpp
        networkview.cpp
        ntp.cpp
        ptp.cpp
        leapseconds.cpp
        updatepackage.cpp
        pkg.cpp
        pkgrecord.cpp
        packages.cpp
        http.cpp
        httpclient.cpp
        shacrypt.cpp
        ghash.cpp
        logtext.cpp
        topview.cpp
        hostname.cpp
        texttemplate.cpp
        textdocument.cpp
        syntaxscan.cpp
        images.cpp
        pixelrepeat.cpp
        imageview.cpp
        notifications.cpp
        synth.cpp
        fmsynth.cpp
        pipe.cpp
        streampair.cpp
        guiprotocol.cpp
        process.cpp
        regex.cpp
        rom.cpp
        result.cpp
        run.cpp
        pty.cpp
        screenconsole.cpp
        shell.cpp
        signals.cpp
        span.cpp
        speech.cpp
        staticobject.cpp
        stats.cpp
        string.cpp
        stringview.cpp
        syntax.cpp
        sysconf.cpp
        terminal.cpp
        terminalscreen.cpp
        terminalselection.cpp
        ui.cpp
        time.cpp
        vfs.cpp
        workerpool.cpp
        ymodem.cpp
        zoneinfo.cpp)
target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROOT}/thirdparty/doctest)
# The network stack's private headers the harness uses (rxcopies.h).
target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROOT}/components/bmds_net/src)
# littlefs's header for littlefs.cpp (devplan/storage.md §13c); its code is bmds_fs's, so the
# same definitions.
target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROOT}/thirdparty/littlefs)
target_compile_definitions(bmds_host_tests PRIVATE
        BMDS_TEST_LEAPSECONDS="${BMDS_ROOT}/thirdparty/tzdata/leap-seconds.list"
        BMDS_TEST_UPDATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/update"
        BMDS_TEST_KEYS_DIR="${BMDS_ROOT}/keys"
        BMDS_TEST_PKG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pkg")
target_compile_definitions(bmds_host_tests PRIVATE LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
target_compile_definitions(bmds_host_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
        BMDS_CORPUS_DIR="${CMAKE_CURRENT_LIST_DIR}/corpus"
        BMDS_PNG_DIR="${CMAKE_CURRENT_LIST_DIR}/png"
        BMDS_IMAGES_DIR="${CMAKE_CURRENT_LIST_DIR}/images"
        BMDS_AUDIO_DIR="${CMAKE_CURRENT_LIST_DIR}/audio"
        BMDS_SYNTH_DIR="${CMAKE_CURRENT_LIST_DIR}/synth"
        BMDS_OPL_BANKS_DIR="${BMDS_ROOT}/thirdparty/opl-banks"
        BMDS_FONTS_DIR="${BMDS_ROOT}/thirdparty/plex/fonts"
        BMDS_FONTS_TEST_DIR="${CMAKE_CURRENT_LIST_DIR}/fonts"
        BMDS_SPEECH_DIR="${BMDS_ROOT}/thirdparty/svox-pico/lang"
        BMDS_COMPAT_DIR="${CMAKE_CURRENT_LIST_DIR}/../compat")
# The time zone database built from thirdparty/tzdata (devplan/time.md §9), as
# the distribution builds it, when the host has zic: its every zone is tested.
find_program(BMDS_ZIC zic PATHS /usr/sbin /sbin)
find_package(Python3 COMPONENTS Interpreter)
if(BMDS_ZIC AND Python3_Interpreter_FOUND)
        file(GLOB BMDS_TZDATA ${BMDS_ROOT}/thirdparty/tzdata/*)
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/zoneinfo.bin
                COMMAND ${CMAKE_COMMAND} -E env ZIC=${BMDS_ZIC} ${Python3_EXECUTABLE} ${BMDS_ROOT}/tools/tzbundle
                        ${BMDS_ROOT}/thirdparty/tzdata ${CMAKE_BINARY_DIR}/zoneinfo.bin
                DEPENDS ${BMDS_TZDATA} ${BMDS_ROOT}/tools/tzbundle
                VERBATIM)
        add_custom_target(bmds_test_zoneinfo DEPENDS ${CMAKE_BINARY_DIR}/zoneinfo.bin)
        add_dependencies(bmds_host_tests bmds_test_zoneinfo)
        target_compile_definitions(bmds_host_tests PRIVATE BMDS_TEST_ZONEINFO="${CMAKE_BINARY_DIR}/zoneinfo.bin")
endif()
# The ROM's parts (devplan/rom.md), as the firmware's build makes them: the bmds icon theme
# from rom/icons.list (its IconId header is bmds_core's), and an image of tests/host/rom with the
# theme in it; the icon names BMDS's .desktop files use are checked to be in the theme.
if(Python3_Interpreter_FOUND)
        set(BMDS_MKROM ${BMDS_ROOT}/tools/mkrom.py)
        set(BMDS_ICONS_LIST ${BMDS_ROOT}/rom/icons.list)
        set(BMDS_ROM_OUT ${CMAKE_BINARY_DIR}/rom)
        file(GLOB_RECURSE BMDS_TEST_ROM_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/rom/*)
        add_custom_command(OUTPUT ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/bmds.LICENSES
                COMMAND ${Python3_EXECUTABLE} ${BMDS_MKROM} icons ${BMDS_ICONS_LIST} ${BMDS_ROOT}
                        ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/bmds.LICENSES
                DEPENDS ${BMDS_MKROM} ${BMDS_ICONS_LIST}
                VERBATIM)
        # With the time zone database when it is built (zic): the ROM's, used without a card.
        set(BMDS_TEST_ROM_ZONES)
        set(BMDS_TEST_ROM_ZONES_FILE)
        if(TARGET bmds_test_zoneinfo)
                set(BMDS_TEST_ROM_ZONES --file share/zoneinfo.bin=${CMAKE_BINARY_DIR}/zoneinfo.bin)
                set(BMDS_TEST_ROM_ZONES_FILE ${CMAKE_BINARY_DIR}/zoneinfo.bin)
        endif()
        add_custom_command(OUTPUT ${BMDS_ROM_OUT}/test.rom ${BMDS_ROM_OUT}/include/romhash.h
                COMMAND ${Python3_EXECUTABLE} ${BMDS_MKROM} image ${BMDS_ROM_OUT}/test.rom
                        --dir =${CMAKE_CURRENT_LIST_DIR}/rom
                        --file images/grey.png=${CMAKE_CURRENT_LIST_DIR}/png/grey.png
                        --file share/icons/bmds.icons=${BMDS_ROM_OUT}/bmds.icons
                        ${BMDS_TEST_ROM_ZONES}
                        --link docs/hello-link=/rom/hello.txt
                        --mappable images/*
                        --hash-header ${BMDS_ROM_OUT}/include/romhash.h
                DEPENDS ${BMDS_MKROM} ${BMDS_TEST_ROM_FILES} ${CMAKE_CURRENT_LIST_DIR}/png/grey.png
                        ${BMDS_ROM_OUT}/bmds.icons ${BMDS_TEST_ROM_ZONES_FILE}
                VERBATIM)
        add_custom_target(bmds_test_rom DEPENDS ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/test.rom)
        add_dependencies(bmds_host_tests bmds_test_rom)
        target_include_directories(bmds_host_tests PRIVATE ${BMDS_ROM_OUT}/include)
        target_compile_definitions(bmds_host_tests PRIVATE BMDS_TEST_ROM="${BMDS_ROM_OUT}/test.rom"
                BMDS_TEST_ICONS="${BMDS_ROM_OUT}/bmds.icons" BMDS_ICONS_LIST="${BMDS_ICONS_LIST}"
                BMDS_ROOT_DIR="${BMDS_ROOT}")
        add_test(NAME rom_icon_names COMMAND ${Python3_EXECUTABLE} ${BMDS_MKROM} check ${BMDS_ICONS_LIST}
                 ${BMDS_ROOT}/flash ${BMDS_ROOT}/dist ${BMDS_ROOT}/rom)
endif()
# libbmdsgui's transport is a given fd on the host (devplan/guiprograms.md §8).
set_source_files_properties(${BMDS_ROOT}/sdk/gui/gui.c PROPERTIES COMPILE_DEFINITIONS BMDS_GUI_HOST)
target_link_libraries(bmds_host_tests PRIVATE bmds_core bmds_bench bmds_kernel bmds_console bmds_shell bmds_pkg bmds_fs bmds_loader bmds_drivers bmds_audio bmds_midi bmds_synth bmds_fonts bmds_speech bmds_input bmds_ui bmds_gui bmds_clipboard
        bmds_sysconf bmds_files bmds_mplayer bmds_sysmon bmds_settings bmds_panel bmds_imageview bmds_net bmds_lwip)
bmds_target_options(bmds_host_tests)

# bmds-elfcheck: the loader's format check as a host tool (tools/elfcheck).
add_executable(bmds-elfcheck ${BMDS_ROOT}/tools/elfcheck/main.cpp hostmemory.cpp)
target_link_libraries(bmds-elfcheck PRIVATE bmds_loader)
bmds_target_options(bmds-elfcheck)

# bmds-synthwav: a MIDI file rendered to WAV through the synths (tools/synthwav,
# devplan/audio.md §26), with libbmdsgame's MIDI file walker.
add_executable(bmds-synthwav ${BMDS_ROOT}/tools/synthwav/main.cpp ${BMDS_ROOT}/sdk/game/smf.c hostmemory.cpp)
target_include_directories(bmds-synthwav PRIVATE ${BMDS_ROOT}/sdk/include)
target_link_libraries(bmds-synthwav PRIVATE bmds_synth)
bmds_target_options(bmds-synthwav)

enable_testing()
add_test(NAME bmds_host_tests COMMAND bmds_host_tests)

# The SDK examples, cross-compiled when Espressif's RISC-V GCC is around
# (on PATH, or in ~/.espressif): the tests then load the real thing.
find_program(BMDS_RISCV_GCC riscv32-esp-elf-gcc)
if(NOT BMDS_RISCV_GCC)
        file(GLOB _bmds_gccs "$ENV{HOME}/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc")
        if(_bmds_gccs)
                list(GET _bmds_gccs 0 BMDS_RISCV_GCC)
        endif()
endif()
if(BMDS_RISCV_GCC)
        include(ExternalProject)
        ExternalProject_Add(sdk_examples
                SOURCE_DIR ${BMDS_ROOT}/sdk/examples
                BINARY_DIR ${CMAKE_BINARY_DIR}/sdk-examples
                CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${BMDS_ROOT}/sdk/cmake/bmds-toolchain.cmake
                           -DCMAKE_BUILD_TYPE=MinSizeRel -DBMDS_GCC=${BMDS_RISCV_GCC}
                BUILD_ALWAYS ON
                INSTALL_COMMAND "")
        add_dependencies(bmds_host_tests sdk_examples)
        target_compile_definitions(bmds_host_tests PRIVATE
                BMDS_SDK_HELLO="${CMAKE_BINARY_DIR}/sdk-examples/hello.elf"
                BMDS_SDK_EXAMPLES="${CMAKE_BINARY_DIR}/sdk-examples")
        add_test(NAME sdk_examples_are_programs
                 COMMAND bmds-elfcheck ${CMAKE_BINARY_DIR}/sdk-examples/hello.elf)
else()
        message(STATUS "riscv32-esp-elf-gcc not found: the SDK examples are not built or tested")
endif()

# angel (programs/angel, devplan/scripting.md): the AngelScript runtime built for the host from
# the same sources as on the board, with the generic calling convention as there
# (AS_MAX_PORTABILITY); the script corpus (tests/scripts) and the script API's lock.
file(GLOB BMDS_AS_SOURCES ${BMDS_ROOT}/thirdparty/angelscript/source/*.cpp)
add_library(bmds_angelscript STATIC ${BMDS_AS_SOURCES} ${BMDS_ROOT}/thirdparty/angelscript/add_on/scriptarray/scriptarray.cpp)
target_include_directories(bmds_angelscript PUBLIC ${BMDS_ROOT}/thirdparty/angelscript/include)
target_compile_definitions(bmds_angelscript PUBLIC AS_MAX_PORTABILITY AS_FLOAT_LITERALS AS_TOP_LEVEL_STATEMENTS
        AS_NO_EXCEPTIONS)
# AngelScript as released compiles with warnings: not ours to fix.
target_compile_options(bmds_angelscript PRIVATE -std=gnu++20 -fno-exceptions -fno-rtti -w)
# libangel (devplan/preso.md §7.9): the runtime, everything in programs/angel but angel's
# main.cpp and debug.cpp. A program gets the parts it reaches: Preso's decks the language's.
file(GLOB BMDS_LIBANGEL_SOURCES CONFIGURE_DEPENDS ${BMDS_ROOT}/programs/angel/*.cpp)
list(REMOVE_ITEM BMDS_LIBANGEL_SOURCES ${BMDS_ROOT}/programs/angel/main.cpp ${BMDS_ROOT}/programs/angel/debug.cpp)
add_library(bmds_libangel STATIC ${BMDS_LIBANGEL_SOURCES})
target_include_directories(bmds_libangel PUBLIC ${BMDS_ROOT}/programs/angel PRIVATE ${BMDS_ROOT}/sdk/include)
target_link_libraries(bmds_libangel PUBLIC bmds_angelscript m)
bmds_target_options(bmds_libangel)
# The drawing is libbmdsgame's; a window on the host is only its picture (no ANGEL_BMDS).
# The GUI's client (libbmdsgui) too: a host has no GUI service, so scripts' GUI objects can't be
# made there, but the bindings are registered (--api, the lock).
add_executable(angel ${BMDS_ROOT}/programs/angel/main.cpp ${BMDS_ROOT}/programs/angel/debug.cpp
        ${BMDS_ROOT}/sdk/game/canvas.c ${BMDS_ROOT}/sdk/game/shapes.c
        ${BMDS_ROOT}/sdk/game/text.c ${BMDS_ROOT}/sdk/gui/gui.c
        angelsys.cpp fakefonts.cpp ${CMAKE_CURRENT_LIST_DIR}/hostmemory.cpp)
target_include_directories(angel PRIVATE ${BMDS_ROOT}/sdk/include)
target_link_libraries(angel PRIVATE bmds_libangel bmds_fonts pthread m)
# The font calls' fake (fakefonts.cpp): the real library over IBM Plex.
target_compile_definitions(angel PRIVATE BMDS_FONTS_DIR="${BMDS_ROOT}/thirdparty/plex/fonts")
bmds_target_options(angel)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
        add_test(NAME angel_scripts COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/run.py $<TARGET_FILE:angel>
                 ${BMDS_ROOT}/tests/scripts)
        add_test(NAME angel_api_lock COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/apilock.py
                 $<TARGET_FILE:angel> ${BMDS_ROOT}/tests/scripts/api.lock)
        add_test(NAME angel_serve COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/serve.py
                 $<TARGET_FILE:angel>)
        add_test(NAME angel_examples COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/examples.py
                 $<TARGET_FILE:angel> ${BMDS_ROOT})
endif()

# Preso (programs/preso, devplan/preso.md): its core built with nothing of BMDS on its include
# path (the rule that keeps it portable, §2), md4c, the command, and the core's tests.
add_library(preso_md4c STATIC ${BMDS_ROOT}/thirdparty/md4c/md4c.c)
target_include_directories(preso_md4c PUBLIC ${BMDS_ROOT}/thirdparty/md4c)
# md4c as released: its warnings aren't ours to fix.
target_compile_options(preso_md4c PRIVATE -w)
file(GLOB PRESO_CORE_SOURCES CONFIGURE_DEPENDS ${BMDS_ROOT}/programs/preso/core/*.cpp)
add_library(preso_core STATIC ${PRESO_CORE_SOURCES} ${BMDS_ROOT}/programs/preso/headless/headless.cpp)
target_link_libraries(preso_core PUBLIC preso_md4c)
bmds_target_options(preso_core)
# Its scripts (programs/preso/script): AngelScript and libangel's language, outside the core.
file(GLOB PRESO_SCRIPT_SOURCES CONFIGURE_DEPENDS ${BMDS_ROOT}/programs/preso/script/*.cpp)
add_library(preso_script STATIC ${PRESO_SCRIPT_SOURCES})
target_link_libraries(preso_script PUBLIC preso_core bmds_libangel)
bmds_target_options(preso_script)
add_executable(preso ${BMDS_ROOT}/programs/preso/main.cpp)
target_link_libraries(preso PRIVATE preso_script)
bmds_target_options(preso)
add_executable(preso_tests preso/main.cpp preso/deck.cpp preso/layout.cpp preso/images.cpp preso/themes.cpp preso/present.cpp preso/scripts.cpp preso/notes.cpp preso/export.cpp)
target_include_directories(preso_tests PRIVATE ${BMDS_ROOT}/thirdparty/doctest ${BMDS_ROOT}/programs/preso)
target_compile_definitions(preso_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_link_libraries(preso_tests PRIVATE preso_script)
bmds_target_options(preso_tests)
add_test(NAME preso_core COMMAND preso_tests)
# The deck profile's script API is userspace ABI too (devplan/preso.md §7.8).
if(Python3_Interpreter_FOUND)
        add_test(NAME preso_api_lock COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/apilock.py
                 $<TARGET_FILE:preso> ${BMDS_ROOT}/tests/preso/api.lock)
endif()

# The corpus's expectations, checked against real bash (when there is one).
find_program(BASH_PROGRAM bash)
find_package(Python3 COMPONENTS Interpreter)
if(BASH_PROGRAM AND Python3_Interpreter_FOUND)
        add_test(NAME corpus_agrees_with_bash
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/corpus/check_bash.py
                         ${CMAKE_CURRENT_LIST_DIR}/corpus)
endif()

# The GUI classes' lock and their generated C wrappers (devplan/guiprograms.md §10).
add_test(NAME gui_lock COMMAND ${Python3_EXECUTABLE} ${BMDS_ROOT}/tests/scripts/guilock.py ${BMDS_ROOT})
