CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g
LDFLAGS = -static
SRC = src/main.c src/toolchain.c src/storage.c
OUT = cup
STATE_FILE = cup_state.txt

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

clean:
	rm -f $(OUT) $(STATE_FILE)

.PHONY: all clean