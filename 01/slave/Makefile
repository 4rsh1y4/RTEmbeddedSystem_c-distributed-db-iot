CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2
LIBS = -lsqlite3

all: slave_node

slave_node: main.cpp mongoose.c
	$(CXX) $(CXXFLAGS) main.cpp mongoose.c -o slave_node $(LIBS)

clean:
	rm -f slave_node