.RECIPEPREFIX := >
.PHONY: all clean

TARGET := pulsar_rt
SRCS := main.cpp
OBJS := $(SRCS:.cpp=.o)

OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)
OPENCV_LIBS := $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null)

ifeq ($(strip $(OPENCV_LIBS)),)
$(error OpenCV not found (opencv4/opencv). Install OpenCV on Jetson.)
endif

CXX := g++
CXXFLAGS := -O3 -pipe -w -std=c++17 -I../Galaxy_camera/inc $(OPENCV_CFLAGS)
LDFLAGS := -L../Galaxy_camera/lib/armv8 -Wl,-rpath,'$$ORIGIN/../Galaxy_camera/lib/armv8' -lgxiapi -lpthread $(OPENCV_LIBS)

all: $(TARGET)

$(TARGET): $(OBJS)
>$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
>$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
>$(RM) $(OBJS) $(TARGET)
