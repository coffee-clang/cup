CC = gcc
PREFIX = $(HOME)/deps/install
PROJECT_ROOT = $(CURDIR)

CFLAGS = -Wall -Wextra -Werror -std=c11 -g -D_POSIX_C_SOURCE=200809L -I$(PREFIX)/include -I$(PROJECT_ROOT)/include
LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX)/lib64

SRC = \
	src/main.c \
	src/options.c \
	src/commands.c \
	src/state.c \
	src/filesystem.c \
	src/manifest.c \
	src/registry.c \
	src/fetch.c \
	src/extract.c \
	src/util.c \
	src/interrupt.c \
	src/platform.c

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