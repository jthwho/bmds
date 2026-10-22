# angel's build (dist/angel/build.sh copies it into the build directory): AngelScript from
# thirdparty/angelscript (its warnings aren't ours to fix) and the runtime from programs/angel.

AS_SOURCES := $(wildcard $(AS)/source/*.cpp) $(AS)/add_on/scriptarray/scriptarray.cpp
ANGEL_SOURCES := $(wildcard $(SRC)/*.cpp)
OBJECTS := $(patsubst %.cpp,as_%.o,$(notdir $(AS_SOURCES))) $(patsubst %.cpp,angel_%.o,$(notdir $(ANGEL_SOURCES)))
FLAGS := -O2 $(DEFINES) -I$(AS)/include

vpath %.cpp $(AS)/source $(AS)/add_on/scriptarray

# The SDK's libraries it links (libbmdsgame's drawing, libbmds): relinked when they change.
SDK_LIBS := $(wildcard $(dir $(CXX))bmds-libc/lib/*.a)

angel: $(OBJECTS) $(SDK_LIBS)
	$(CXX) -o $@ $(OBJECTS)

as_%.o: %.cpp
	$(CXX) $(FLAGS) -w -c $< -o $@

angel_%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h)
	$(CXX) $(FLAGS) -Wall -Wextra -Werror -Wshadow -c $< -o $@
