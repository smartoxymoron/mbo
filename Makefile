CXX = g++
CXXFLAGS = -std=c++20 -O3 -mavx2 -Wall -Wextra -DNDEBUG
#CXXFLAGS = -std=c++20 -O3 -mavx2 -Wall -Wextra -Wconversion -Wsign-conversion -DNDEBUG
LDFLAGS = 

mbo: mbo.cpp perfprofiler.h
	$(CXX) $(CXXFLAGS) -I./boost_1_87_0 -g -o mbo mbo.cpp $(LDFLAGS)

clean:
	rm -f mbo output.bin

run: mbo
	./mbo test_data.bin

.PHONY: clean run


