# preso's build (dist/preso/build.sh copies it into the build directory): md4c from
# thirdparty/md4c and AngelScript from thirdparty/angelscript (their warnings aren't ours to
# fix), libangel from programs/angel (all of angel but its main.cpp and debug.cpp: the linker
# takes the parts Preso reaches), and Preso from programs/preso: its core, its scripts and the
# BMDS platform. AngelScript is built as angel builds it: the same language.

AS_DEFINES := -DAS_TOP_LEVEL_STATEMENTS -DAS_NO_EXCEPTIONS -DAS_FLOAT_LITERALS
AS_SOURCES := $(wildcard $(AS)/source/*.cpp) $(AS)/add_on/scriptarray/scriptarray.cpp
LIBANGEL_SOURCES := $(filter-out $(ANGEL)/main.cpp $(ANGEL)/debug.cpp,$(wildcard $(ANGEL)/*.cpp))
CORE_SOURCES := $(wildcard $(SRC)/core/*.cpp)
SCRIPT_SOURCES := $(wildcard $(SRC)/script/*.cpp)
PRESO_HEADERS := $(wildcard $(SRC)/core/*.h $(SRC)/script/*.h $(SRC)/bmds/*.h $(ANGEL)/*.h)
AS_OBJECTS := $(patsubst %.cpp,as_%.o,$(notdir $(AS_SOURCES)))
LIBANGEL_OBJECTS := $(patsubst %.cpp,angel_%.o,$(notdir $(LIBANGEL_SOURCES)))
OBJECTS := md4c.o $(patsubst $(SRC)/core/%.cpp,preso_core_%.o,$(CORE_SOURCES)) \
	$(patsubst $(SRC)/script/%.cpp,preso_script_%.o,$(SCRIPT_SOURCES)) preso_bmds_bmdsplatform.o preso_main.o
FLAGS := -O2 -I$(MD4C) -I$(SRC) -I$(ANGEL) -I$(AS)/include $(AS_DEFINES) -DPRESO_BMDS -DPRESO_VERSION=\"$(VERSION)\"
WARNINGS := -Wall -Wextra -Werror -Wshadow

vpath %.cpp $(AS)/source $(AS)/add_on/scriptarray

# The SDK's libraries it links: relinked when they change.
SDK_LIBS := $(wildcard $(dir $(CXX))bmds-libc/lib/*.a)

preso: $(OBJECTS) libangel.a libangelscript.a $(SDK_LIBS)
	$(CXX) -o $@ $(OBJECTS) libangel.a libangelscript.a

libangelscript.a: $(AS_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

libangel.a: $(LIBANGEL_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

md4c.o: $(MD4C)/md4c.c $(MD4C)/md4c.h
	$(CC) -O2 -w -I$(MD4C) -c $< -o $@

as_%.o: %.cpp
	$(CXX) -O2 $(AS_DEFINES) -I$(AS)/include -w -c $< -o $@

angel_%.o: $(ANGEL)/%.cpp $(wildcard $(ANGEL)/*.h)
	$(CXX) -O2 $(AS_DEFINES) -DANGEL_BMDS -I$(AS)/include $(WARNINGS) -c $< -o $@

preso_core_%.o: $(SRC)/core/%.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

preso_script_%.o: $(SRC)/script/%.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

preso_bmds_%.o: $(SRC)/bmds/%.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@

preso_main.o: $(SRC)/main.cpp $(PRESO_HEADERS)
	$(CXX) $(FLAGS) $(WARNINGS) -c $< -o $@
