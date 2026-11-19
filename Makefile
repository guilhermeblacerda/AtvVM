CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
TARGET = vm

all: $(TARGET)

$(TARGET): vm.c
	$(CC) $(CFLAGS) -o $(TARGET) vm.c

clean:
	rm -f $(TARGET) correct.txt

.PHONY: all clean