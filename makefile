CC = cc
CCFLAGS = -Wall -Wextra -Wno-implicit-fallthrough
SOURCE = ./editor.c
TARGET = ./editor

.PHONY: all clean

all: $(TARGET)
$(TARGET): $(SOURCE)
	$(CC) $(CCFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

