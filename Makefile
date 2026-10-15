CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -s
TARGET = login_portal
SRC = login_portal.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	./install.sh

uninstall:
	./uninstall.sh
