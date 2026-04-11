CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g
LDFLAGS = -static
SRC = src/main.c src/component.c src/state.c src/fs.c
OUT = cup

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

clean:
	rm -f $(OUT)

dev-clean: clean
	rm -rf ~/.cup
	clear

.PHONY: all clean dev-clean