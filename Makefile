CXX = g++
CXXFLAGS = -std=c++20 -O3 -mavx2 -Wall -Wextra
LDFLAGS = 

mbo: mbo.cpp perfprofiler.h
	$(CXX) $(CXXFLAGS) -o mbo mbo.cpp $(LDFLAGS)

clean:
	rm -f mbo output.bin

run: mbo
	./mbo test_data.bin

.PHONY: clean run


