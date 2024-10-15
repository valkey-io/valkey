# Author: Roshan Swain <swainroshan001@gmail.com>

CXX?=g++
CXXFLAGS=-std=c++11 -O3 -fPIC
AR=ar
ARFLAGS=rcs

SOURCES=fast_float_strtod.cpp
OBJECTS=$(SOURCES:.cpp=.o)
TARGET=libfast_float.a

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean