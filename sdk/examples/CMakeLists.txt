# The SDK examples. Configure with the SDK toolchain file, then build:
#   cmake -S sdk/examples -B build-sdk -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/bmds-toolchain.cmake

cmake_minimum_required(VERSION 3.16)
project(bmds_examples C ASM)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BmdsProgram.cmake)

bmds_add_program(hello SOURCES hello/hello.c ICONS hello/hello-16.png hello/hello-32.png)
bmds_add_program(cat SOURCES cat/cat.c)
bmds_add_program(ls SOURCES ls/ls.c)
bmds_add_program(conf SOURCES conf/conf.c)

# Test programs (not examples): the loader's board tests use them.
bmds_add_program(progtest SOURCES ../tests/progtest.c)
