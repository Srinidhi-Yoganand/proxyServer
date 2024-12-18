CC = g++
CFLAGS = -Wall -pthread -g

LDFLAGS = -lz

TARGET = proxy_server

SRC = server.c proxy_parse.c 

OBJ = $(SRC: .c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f proxy_server *.o

run: $(TARGET)
	./$(TARGET) 8080

rebuild: clean all