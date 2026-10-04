# Fails the build if the linker map shows code BMDS must never link.
#
#   cmake -DMAP=<bmds.map> -P cmake/check_map.cmake
#
# libstdc++'s exception runtime (eh_alloc.o and friends) gets in through any
# reference to a throwing helper, e.g. std::string_view::substr's
# __throw_out_of_range_fmt, even from code that is never called: its static
# constructor keeps it alive through --gc-sections. It costs ~5.7 KB of flash
# and ~200 B of DIRAM, and it means -fno-exceptions code calls something that
# can throw.

file(READ "${MAP}" map)
foreach(member eh_alloc.o functexcept.o cow-stdexcept.o)
        string(FIND "${map}" "libstdc++.a(${member})" found)
        if(NOT found EQUAL -1)
                message(FATAL_ERROR
                        "libstdc++ ${member} is linked: something references a throwing "
                        "standard-library helper. Find it in ${MAP} under 'Archive member "
                        "included to satisfy reference' and replace the call.")
        endif()
endforeach()
