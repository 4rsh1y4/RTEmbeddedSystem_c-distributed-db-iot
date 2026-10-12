# کامپایلر و فلگ‌های بهینه‌سازی
CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2

# کتابخانه‌هایی که باید لینک شوند
LIBS = -lsqlite3

# هدف نهایی
all: master_node

master_node: main.cpp mongoose.c
	$(CXX) $(CXXFLAGS) main.cpp mongoose.c -o master_node $(LIBS)

clean:
	rm -f master_node