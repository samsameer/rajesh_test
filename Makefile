CC      ?= cc
AR      ?= ar
BUILD   ?= build

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wdouble-promotion -pthread
LDLIBS   += -lm -pthread

LIB_SRCS := src/spsc_queue.c src/clock.c src/backoff.c src/fusion.c \
            src/sensor.c src/aggregator.c src/writer.c
LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libsensorfusion.a

.PHONY: all run clean

all: $(BUILD)/sensor_fusion

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/sensor_fusion: $(BUILD)/src/main.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

run: $(BUILD)/sensor_fusion
	$(BUILD)/sensor_fusion $(ARGS)

clean:
	rm -rf build build-*

-include $(LIB_OBJS:.o=.d) $(BUILD)/src/main.d
