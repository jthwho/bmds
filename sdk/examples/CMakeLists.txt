# The SDK examples. Configure with the SDK toolchain file, then build:
#   cmake -S sdk/examples -B build-sdk -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/bmds-toolchain.cmake

cmake_minimum_required(VERSION 3.16)
project(bmds_examples C CXX ASM)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BmdsProgram.cmake)

bmds_add_program(hello SOURCES hello/hello.c ICONS hello/hello-16.png hello/hello-32.png)
bmds_add_program(cat SOURCES cat/cat.c)
bmds_add_program(ls SOURCES ls/ls.c)
bmds_add_program(conf SOURCES conf/conf.c)
bmds_add_program(tone SOURCES tone/tone.c)
bmds_add_program(player SOURCES player/player.c)
bmds_add_program(keytone SOURCES keytone/keytone.c)
bmds_add_program(midimon SOURCES midimon/midimon.c)
bmds_add_program(speak SOURCES speak/speak.c)
bmds_add_program(surfdemo SOURCES surfdemo/surfdemo.c DISPLAY)
bmds_add_program(appinfo SOURCES appinfo/appinfo.c)
bmds_add_program(bounce SOURCES bounce/bounce.c DISPLAY)
bmds_add_program(cxxdemo SOURCES cxxdemo/cxxdemo.cpp)
bmds_add_program(guidemo SOURCES guidemo/guidemo.c)
bmds_add_program(guigallery SOURCES guigallery/guigallery.c)
bmds_add_program(guisurface SOURCES guisurface/guisurface.c DISPLAY)
bmds_add_program(guipanel SOURCES guipanel/guipanel.c)

# Application folders (devplan/apps.md): copied into a card's apps/, each a command in /apps.
bmds_add_app(surfdemo PROGRAM surfdemo DESKTOP surfdemo/app.desktop)
bmds_add_app(appinfo PROGRAM appinfo DESKTOP appinfo/app.desktop)

# Test programs (not examples): the loader's board tests use them.
bmds_add_program(progtest SOURCES ../tests/progtest.c)
bmds_add_program(blittest SOURCES ../tests/blittest.c)
bmds_add_program(guitest SOURCES ../tests/guitest.c)
