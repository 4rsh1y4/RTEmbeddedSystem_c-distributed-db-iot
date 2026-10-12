CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LIBS = -lsqlite3 -lcurl

TARGET = alert_daemon
SOURCE = main.cpp

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)