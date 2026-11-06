# angel's build (dist/angel/build.sh copies it into the build directory): AngelScript from
# thirdparty/angelscript (its warnings aren't ours to fix), the runtime from programs/angel, and
# its stage (devplan/preso.md §8c): Preso's scene runtime from programs/preso (its core, its
# scripts, the BMDS platform) and md4c, as an archive, so the linker takes only what the stage
# reaches.

AS_SOURCES := $(wildcard $(AS)/source/*.cpp) $(AS)/add_on/scriptarray/scriptarray.cpp
ANGEL_SOURCES := $(wildcard $(SRC)/*.cpp)
OBJECTS := $(patsubst %.cpp,as_%.o,$(notdir $(AS_SOURCES))) $(patsubst %.cpp,angel_%.o,$(notdir $(ANGEL_SOURCES)))
SCENE_OBJECTS := $(patsubst $(PRESO)/core/%.cpp,preso_core_%.o,$(wildcard $(PRESO)/core/*.cpp)) \
	$(patsubst $(PRESO)/script/%.cpp,preso_script_%.o,$(wildcard $(PRESO)/script/*.cpp)) \
	preso_bmds_bmdsplatform.o md4c.o
HEADERS := $(wildcard $(SRC)/*.h $(PRESO)/core/*.h $(PRESO)/script/*.h $(PRESO)/bmds/*.h)
FLAGS := -O2 $(DEFINES) -I$(AS)/include -I$(SRC)
WARNINGS := -Wall -Wextra -Werror -Wshadow

vpath %.cpp $(AS)/source $(AS)/add_on/scriptarray

# The SDK's libraries it links (libbmdsgame's drawing, libbmds): relinked when they change.
SDK_LIBS := $(wildcard $(dir $(CXX))bmds-libc/lib/*.a)

angel: $(OBJECTS) libscene.a $(SDK_LIBS)
	$(CXX) -o $@ $(OBJECTS) libscene.a

libscene.a: $(SCENE_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

as_%.o: %.cpp
	$(CXX) $(FLAGS) -w -c $< -o $@

angel_%.o: $(SRC)/%.cpp $(HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

preso_core_%.o: $(PRESO)/core/%.cpp $(HEADERS)
	$(CXX) $(FLAGS) -I$(MD4C) $(WARNINGS) -c $< -o $@

preso_script_%.o: $(PRESO)/script/%.cpp $(HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

preso_bmds_%.o: $(PRESO)/bmds/%.cpp $(HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

md4c.o: $(MD4C)/md4c.c $(MD4C)/md4c.h
	$(CC) -O2 -w -I$(MD4C) -c $< -o $@
