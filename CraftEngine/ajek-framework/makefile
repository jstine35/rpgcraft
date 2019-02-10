src = $(wildcard ./src/*.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)  # one dependency file for each source

LDFLAGS = -lpng -lz -lm

.RECIPEPREFIX = >

CXXFLAGS  = -DUSE_PRECOMPILED_HEADER=1
CXXFLAGS += -DTARGET_DEBUG=1
CXXFLAGS += -DHAS_RDTSC=1
CXXFLAGS += -DHAS_READEFLAGS=1
CXXFLAGS += -DHAS_MM_UNDEFINED=1
CXXFLAGS += -I./h

libAjekFramework: $(obj)
> $(CXX) -o $@ $^ $(LDFLAGS)

-include $(dep)   # include all dep files in the makefile

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cpp
> @$(CPP) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

.PHONY: clean
clean:
> rm -f $(obj) mygame

.PHONY: cleandep
cleandep:
> rm -f $(dep)
