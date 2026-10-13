# SVOX Pico (thirdparty/svox-pico, devplan/speech.md §3): its sources, include directory and
# settings, shared by this component's CMakeLists.txt, tests/host and tests/fuzz.
#
# PICO_NO_FILES: Pico opens no files (bmds-pal.diff); voices are given to it in memory.
# -fwrapv: Pico's fixed-point signal code lets signed products wrap (UBSan reports overflow on
# every sentence, picosig.c, picosig2.c), as the compilers it was written for did; defined here.
# Its warnings are upstream's (the fallthroughs are meant; -Wstringop-overflow is a false alarm on
# its markup parameters); the fuzzers find what matters.

set(BMDS_PICO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/svox-pico/lib)
set(BMDS_PICO_SRCS)
# picodbg.c and picokdbg.c hold nothing without PICO_DEBUG.
foreach(name picoacph picoapi picobase picocep picoctrl picodata picoextapi picofftsg picokdt picokfst
             picoklex picoknow picokpdf picokpr picoktab picoos picopal picopam picopr picorsrc picosa picosig picosig2 picospho picotok picotrns picowa)
        list(APPEND BMDS_PICO_SRCS ${BMDS_PICO_DIR}/${name}.c)
endforeach()
set(BMDS_PICO_INCLUDES ${BMDS_PICO_DIR})
set(BMDS_PICO_DEFINITIONS PICO_NO_FILES)
set(BMDS_PICO_OPTIONS -fwrapv -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-unused-but-set-variable
                      -Wno-unused-function -Wno-stringop-overflow -Wno-sign-compare -Wno-shadow)
# The signal generation at -O2 (most of the time; the rest -Os, as the firmware is). Measured on the
# board: 4.63 s for 24.5 s of speech at -Os, 3.96-4.15 here (+12 KB), 3.84-4.03 all -O2 (+29 KB).
set(BMDS_PICO_FAST_SRCS)
foreach(name picocep picofftsg picosig picosig2 picopam)
        list(APPEND BMDS_PICO_FAST_SRCS ${BMDS_PICO_DIR}/${name}.c)
endforeach()
