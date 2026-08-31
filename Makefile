CXX = g++

CXXFLAGS = -Wall -O2 -pthread -std=c++17

TARGET = httpserver

SRC = http.cpp ThreadPool.cpp EpollEngine.cpp MimeTypes.cpp

all:
	$(CXX) $(SRC) -o $(TARGET) $(CXXFLAGS)

clean:
	rm -f $(TARGET)