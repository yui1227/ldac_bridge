# Build with MSYS2 UCRT64:  ./build.sh   (or mingw32-make BTSTACK=... LIBLDAC=...)
#
# build.sh passes 8.3 short paths so that deep source trees stay below the
# Windows 260 character path limit. Object files are flat in build/ for the same reason.
BTSTACK ?= ../third_party/btstack
LIBLDAC ?= ../third_party/libldac

CC      = gcc
CFLAGS  = -O2 -g -Wall -Wno-unused-function -Wno-format-truncation \
          -I. \
          -I$(BTSTACK)/src \
          -I$(BTSTACK)/platform/windows \
          -I$(BTSTACK)/platform/posix \
          -I$(BTSTACK)/platform/embedded \
          -I$(BTSTACK)/chipset/intel \
          -I$(BTSTACK)/3rd-party/bluedroid/encoder/include \
          -I$(BTSTACK)/3rd-party/bluedroid/decoder/include \
          -I$(BTSTACK)/3rd-party/yxml \
          -I$(BTSTACK)/3rd-party/md5 \
          -I$(LIBLDAC)/inc
LDLIBS  = -lsetupapi -lwinusb -lole32 -lwinmm -static

SRC_APP      = main.c ldac_source.c pcm_capture.c media_keys.c
SRC_BTSTACK  = $(filter-out %lc3_google.c %lc3plus_fraunhofer.c,$(wildcard $(BTSTACK)/src/*.c)) \
               $(wildcard $(BTSTACK)/src/classic/*.c) \
               $(filter-out %hci_dump_windows_stdout.c,$(wildcard $(BTSTACK)/platform/windows/*.c)) \
               $(wildcard $(BTSTACK)/chipset/intel/*.c) \
               $(wildcard $(BTSTACK)/3rd-party/bluedroid/encoder/srce/*.c) \
               $(wildcard $(BTSTACK)/3rd-party/bluedroid/decoder/srce/*.c) \
               $(BTSTACK)/3rd-party/yxml/yxml.c $(BTSTACK)/3rd-party/md5/md5.c
SRC_LDAC     = $(LIBLDAC)/src/ldaclib.c $(LIBLDAC)/src/ldacBT.c

SRC_LIB      = $(SRC_BTSTACK) $(SRC_LDAC)
OBJ_APP      = $(addprefix build/,$(SRC_APP:.c=.o))
OBJ_LIB      = $(addprefix build/,$(notdir $(SRC_LIB:.c=.o)))

vpath %.c $(sort $(dir $(SRC_APP) $(SRC_LIB)))

all: ldac_bridge.exe

# static library: only modules actually referenced get linked
build/libbtstack.a: $(OBJ_LIB)
	@rm -f $@
	@ar rcs $@ $^

ldac_bridge.exe: $(OBJ_APP) build/libbtstack.a
	$(CC) -o $@ $^ $(LDLIBS)

build/%.o: %.c | build
	@echo CC $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

build:
	@mkdir -p build

offline_test.exe: build/offline_test.o build/pcm_capture.o build/libbtstack.a
	$(CC) -o $@ $^ $(LDLIBS)

build/offline_test.o: tests/offline_test.c | build
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build ldac_bridge.exe

.PHONY: all clean
