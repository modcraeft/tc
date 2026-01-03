CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -I. -I/usr/include/SDL2
LDFLAGS = -lSDL2 -lSDL2_ttf -lm  # SDL_ttf for fonts.
SOURCES = main.c
OBJECTS = $(SOURCES:.c=.o)
EXEC = tc

all: $(EXEC)

$(EXEC): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(EXEC) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(EXEC)

.PHONY: all clean
