SRC = exif.c sample_main.c
OBJ = $(SRC:.c=.o)
TARGET = exif
CFLAGS = -Wall
CC = gcc

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $^

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJ) $(TARGET)

