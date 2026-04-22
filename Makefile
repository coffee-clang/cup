CC = gcc
PREFIX = $(HOME)/deps/install

CFLAGS = -Wall -Wextra -Werror -std=c11 -g -I$(PREFIX)/include
LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX)/lib64

SRC = src/main.c src/component.c src/state.c src/fs.c src/manifest.c src/support.c src/fetch.c src/archive.c src/util.c
TARGET = cup

LDLIBS = -lcurl -larchive -lssl -lcrypto -lz -llzma -ldl

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) -static $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

dev-clean: clean
	rm -rf ~/.cup
	clear