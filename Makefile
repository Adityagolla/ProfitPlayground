# Simple Makefile for Windows (MinGW) and Linux/macOS
# Targets:
#   make            # build demo/pipeline_demo.exe (or .out on *nix)
#   make run        # build and run
#   make dll        # build bridge/libpipeline.dll (needs a 64-bit gcc — see below)
#   make clean      # remove objects and binaries

.PHONY: all run dll clean

CC      := gcc
CFLAGS  := -O2 -Wall -Wno-unused-function -Iengine -Ipipeline -Ibridge
LDFLAGS :=

# The DLL is loaded via ctypes into a 64-bit Python (api/.venv), so it must
# be built with a 64-bit compiler. The plain 32-bit MinGW.org `gcc` above is
# fine for the demo .exe but produces a DLL Python can't load (WinError 193).
# Default: zig cc (clang) pulled from PyPI through uv — no toolchain install
# needed beyond uv itself. Override with any x86_64 gcc/clang, e.g.:
#   make dll CC64="x86_64-w64-mingw32-gcc"
# Note: the Anaconda m2w64-toolchain gcc 5.3 ICEs (segfault) on any code
# using doubles on this machine — don't point CC64 at it.
CC64    ?= uv run --project api --with ziglang python -m ziglang cc -target x86_64-windows-gnu

ENGINE_SRCS := \
	engine/orderbook.c \
	engine/matching_engine.c \
	engine/orders.c

PIPE_SRCS := \
	pipeline/gateway.c \
	pipeline/risk.c \
	pipeline/portfolio.c \
	pipeline/event_bus.c \
	pipeline/pipeline.c

BRIDGE_SRCS := bridge/bridge.c

DEMO_SRCS := demo/pipeline_demo.c

ALL_SRCS := $(ENGINE_SRCS) $(PIPE_SRCS) $(DEMO_SRCS) $(BRIDGE_SRCS)

# Default target
all: demo/pipeline_demo.exe

# Link straight from sources (keeps it simple/cross-shell on Windows)
demo/pipeline_demo.exe: $(ENGINE_SRCS) $(PIPE_SRCS) $(DEMO_SRCS)
	$(CC) $(CFLAGS) -o $@ $(DEMO_SRCS) $(PIPE_SRCS) $(ENGINE_SRCS) $(LDFLAGS)

run: all
	./demo/pipeline_demo.exe

# Shared library consumed by api/engine_bridge.py via ctypes.
dll: bridge/libpipeline.dll

bridge/libpipeline.dll: $(ENGINE_SRCS) $(PIPE_SRCS) $(BRIDGE_SRCS)
	$(CC64) $(CFLAGS) -shared -o $@ $(BRIDGE_SRCS) $(PIPE_SRCS) $(ENGINE_SRCS) $(LDFLAGS)

clean:
	-@del /q engine\*.o 2> NUL || true
	-@del /q pipeline\*.o 2> NUL || true
	-@del /q demo\*.o 2> NUL || true
	-@del /q bridge\*.o 2> NUL || true
	-@del /q demo\pipeline_demo.exe 2> NUL || true
	-@del /q bridge\libpipeline.dll 2> NUL || true
	-@rm -f engine/*.o pipeline/*.o demo/*.o bridge/*.o demo/pipeline_demo.exe bridge/libpipeline.dll 2> /dev/null || true
