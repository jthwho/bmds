# preso's build (dist/preso/build.sh copies it into the build directory): md4c from
# thirdparty/md4c (its warnings aren't ours to fix) and Preso from programs/preso: its core and
# the BMDS platform.

CORE_SOURCES := $(wildcard $(SRC)/core/*.cpp)
PRESO_HEADERS := $(wildcard $(SRC)/core/*.h $(SRC)/bmds/*.h)
OBJECTS := md4c.o $(patsubst $(SRC)/core/%.cpp,preso_core_%.o,$(CORE_SOURCES)) preso_bmds_bmdsplatform.o preso_main.o
FLAGS := -O2 -I$(MD4C) -I$(SRC) -DPRESO_BMDS -DPRESO_VERSION=\"$(VERSION)\"

# The SDK's libraries it links: relinked when they change.
SDK_LIBS := $(wildcard $(dir $(CXX))bmds-libc/lib/*.a)

preso: $(OBJECTS) $(SDK_LIBS)
	$(CXX) -o $@ $(OBJECTS)

md4c.o: $(MD4C)/md4c.c $(MD4C)/md4c.h
	$(CC) -O2 -w -I$(MD4C) -c $< -o $@

preso_core_%.o: $(SRC)/core/%.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) -Wall -Wextra -Werror -Wshadow -c $< -o $@

preso_bmds_%.o: $(SRC)/bmds/%.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) -Wall -Wextra -Werror -Wshadow -c $< -o $@

preso_main.o: $(SRC)/main.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) -Wall -Wextra -Werror -Wshadow -c $< -o $@
