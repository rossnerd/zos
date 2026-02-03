CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g -I./include
# Seznam zdrojových souborů
SRC = src/main.c src/fs_core.c
# Generování seznamu objektových souborů (.o)
OBJ = $(SRC:.c=.o)
# Název výsledné binárky
TARGET = fs_app

# Výchozí pravidlo
all: $(TARGET)

# Linkování
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# Kompilace .c na .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Vyčištění projektu
clean:
	rm -f src/*.o $(TARGET)