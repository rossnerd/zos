CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g -I./include

# ZMĚNA: Seznam všech nových .c souborů
SRC = src/main.c \
      src/fs_utils.c \
      src/cmd_system.c \
      src/cmd_dir.c \
      src/cmd_file.c \
      src/cmd_extra.c

OBJ = $(SRC:.c=.o)
TARGET = fs_app

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(TARGET)