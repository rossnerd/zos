CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g -I./include
# ZMĚNA: Přidány nové soubory
SRC = src/main.c src/fs_ops.c src/fs_utils.c
OBJ = $(SRC:.c=.o)
TARGET = fs_app

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(TARGET)