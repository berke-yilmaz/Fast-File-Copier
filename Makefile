CC = gcc                   
CFLAGS = -Wall -Wextra -O2  
LDFLAGS = -lpthread -lm     

TARGET = MWCp

SRC = 200104004053_main.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
