# Sokol Highway - macOS (Metal) build
CC      ?= clang
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-missing-field-initializers -Ithird_party/sokol -MMD -MP
OBJCFLAGS = -fobjc-arc
FRAMEWORKS = -framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit -framework ImageIO -framework CoreGraphics

BUILD   = build
TARGET  = $(BUILD)/highway
C_SRCS  = src/main.c src/mesh.c src/world.c src/city.c src/textures.c src/cars.c src/present.c src/chart.c
OBJS    = $(C_SRCS:src/%.c=$(BUILD)/%.o) $(BUILD)/sokol_impl.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(FRAMEWORKS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sokol_impl.o: src/sokol_impl.m | $(BUILD)
	$(CC) -O2 -Ithird_party/sokol $(OBJCFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
-include $(OBJS:.o=.d)
