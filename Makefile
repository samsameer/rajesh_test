CC      ?= cc
AR      ?= ar
BUILD   ?= build

OPT      ?= -O2 -g
SF_CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
SF_CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
               -Wmissing-prototypes -Wdouble-promotion -pthread
ALL_CFLAGS   = $(SF_CPPFLAGS) $(CPPFLAGS) $(SF_CFLAGS) $(OPT) $(CFLAGS)
LINK_FLAGS   = $(SF_CFLAGS) $(OPT) $(CFLAGS) $(LDFLAGS)
LDLIBS      += -lm -pthread

LIB_SRCS := src/spsc_queue.c src/clock.c src/backoff.c src/fusion.c \
            src/sensor.c src/aggregator.c src/writer.c
LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libsensorfusion.a

APP      := $(BUILD)/sensor_fusion
TOOL     := $(BUILD)/fusion_file
TESTS    := $(BUILD)/test_fusion $(BUILD)/test_pipeline

ALL_OBJS := $(LIB_OBJS) $(BUILD)/src/main.o $(BUILD)/tools/fusion_file.o \
            $(BUILD)/tests/test_fusion.o $(BUILD)/tests/test_pipeline.o

.PHONY: all test run sanitize tsan clean

all: $(APP) $(TOOL) $(TESTS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(APP): $(BUILD)/src/main.o $(LIB)
	$(CC) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

$(TOOL): $(BUILD)/tools/fusion_file.o $(LIB)
	$(CC) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_%: $(BUILD)/tests/test_%.o $(LIB)
	$(CC) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

test: $(TOOL) $(TESTS)
	$(BUILD)/test_fusion
	$(BUILD)/test_pipeline
	./tests/run_file_tests.sh $(BUILD)/fusion_file

run: $(APP)
	$(APP) $(ARGS)

sanitize:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" test

tsan:
	$(MAKE) BUILD=build-tsan OPT="-O1 -g -fsanitize=thread" test

clean:
	rm -rf build build-asan build-tsan

-include $(ALL_OBJS:.o=.d)
