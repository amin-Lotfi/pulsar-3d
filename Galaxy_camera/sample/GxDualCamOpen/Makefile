.PHONY: all clean

NAME := GxDualCamOpen

CXX := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -I../../inc
LDFLAGS := -L../../lib/armv8 -Wl,-rpath,'$$ORIGIN/../../lib/armv8' -lgxiapi -lpthread

SRCS := GxDualCamOpen.cpp
OBJS := $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	$(RM) $(OBJS) $(NAME)
