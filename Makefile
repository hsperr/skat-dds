# Build the shared library: build/libskatdds.{dylib,so}
#   make                  optimized for this CPU (-march=native)
#   make PORTABLE=1       no CPU-specific flags (for binaries you copy to other machines)
#   make test             C example + Python tests
CXX      ?= c++
CC       ?= cc
UNAME    := $(shell uname)
EXT      := $(if $(filter Darwin,$(UNAME)),dylib,so)
LIB      := build/libskatdds.$(EXT)

ifeq ($(PORTABLE),1)
ARCH :=
else
ARCH := $(shell $(CXX) -march=native -x c++ -fsyntax-only /dev/null 2>/dev/null && echo -march=native || echo -mcpu=native)
endif

CXXFLAGS ?= -O3 $(ARCH)
CXXFLAGS += -std=c++17 -Wall -Wextra -fPIC -Iinclude
LDFLAGS  += -shared -lpthread
ifeq ($(UNAME),Darwin)
LDFLAGS  += -install_name @rpath/libskatdds.dylib
endif

PYTHON ?= python3

all: $(LIB)

$(LIB): src/skatdds.cpp include/skatdds.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/skatdds.cpp $(LDFLAGS)

build/example: examples/example.c $(LIB)
	$(CC) -O2 -Iinclude -o $@ examples/example.c -Lbuild -lskatdds -Wl,-rpath,$(CURDIR)/build

test: $(LIB) build/example
	./build/example
	$(PYTHON) -m pytest -q tests

bench: $(LIB)
	$(PYTHON) bench/bench.py

clean:
	rm -rf build

.PHONY: all test bench clean
