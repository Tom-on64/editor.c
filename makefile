CC = cc
CCFLAGS = -Wall -Wextra
SOURCE = ./editor.c
TARGET = ./editor

.PHONY: all clean

all: $(TARGET)
$(TARGET): $(SOURCE)
	$(CC) $(CCFLAGS) $< -o $@

clean:
	rm $(TARGET)

