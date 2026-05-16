CC = g++

CFLAGS = -Wall -pthread

TARGET = httpserver

SRC = http.cpp 线程池.cpp EpollEngine.cpp

all:
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS)

clean:
	rm -f $(TARGET)