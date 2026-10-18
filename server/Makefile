CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -lpthread -lrt
TARGET ?= aesdsocket
all: $(TARGET)
default: $(TARGET)
$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
clean:
	rm -f $(TARGET) *.o
