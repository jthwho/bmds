# bmds_shell — the command shell and its built-in commands.
#
# The shell core and argument parsing are pure C++ (no ESP-IDF headers) and are
# also built as a plain CMake library for the host tests (tests/host). The
# linker-built command table, shell tasks and built-ins are target-only.

set(BMDS_SHELL_PURE_SRCS
        src/arith.cpp
        src/commandcontext.cpp
        src/exec.cpp
        src/executor.cpp
        src/filecommands.cpp
        src/fscommands.cpp
        src/glob.cpp
        src/iobuiltins.cpp
        src/jobs.cpp
        src/langbuiltins.cpp
        src/parser.cpp
        src/regex.cpp
        src/shell.cpp
        src/syntaxdump.cpp
        src/sysconfcommand.cpp
        src/termcommand.cpp
        src/textcommands.cpp
        src/timecommands.cpp
        src/transfercommands.cpp
        src/variables.cpp
        src/ymodem.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        # WHOLE_ARCHIVE: nothing references builtins.cpp by symbol -- its commands
        # are found through the linker section -- so without it the linker would
        # never pull that object out of the archive.
        idf_component_register(SRCS ${BMDS_SHELL_PURE_SRCS}
                                    src/builtins.cpp
                                    src/notifycommand.cpp
                                    src/shellesp.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel bmds_console bmds_fs bmds_sysconf
                               PRIV_REQUIRES freertos
                               LDFRAGMENTS linker.lf
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_shell STATIC ${BMDS_SHELL_PURE_SRCS})
        target_include_directories(bmds_shell PUBLIC include)
        target_link_libraries(bmds_shell PUBLIC bmds_core bmds_kernel bmds_console bmds_fs bmds_sysconf)
        bmds_target_options(bmds_shell)
endif()
