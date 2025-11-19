OUT?=ddb_analysis_GTK3.so

ESSENTIA_PREFIX?=/usr/local

GCC?=g++
CXXFLAGS?= -std=c++17 -fPIC -O2
LDFLAGS?= -shared

CXXFLAGS+=-I $(ESSENTIA_PREFIX)/include -I /usr/include/eigen3
CXXFLAGS += $(shell pkg-config --cflags gtk+-3.0)

LDLIBS += -lavutil -lavformat -lavcodec -lswresample -lpthread -lm
LDLIBS += -lsamplerate -ltag -lchromaprint -lfftw3f
LDLIBS += $(shell pkg-config --libs gtk+-3.0)

ESSENTIA = $(ESSENTIA_PREFIX)/lib/libessentia.a

SOURCES?=$(wildcard *.cpp)

build:
		$(GCC) $(CXXFLAGS) $(LDFLAGS) -o $(OUT) $(SOURCES) $(LDLIBS) $(ESSENTIA)

install:
		cp $(OUT) /usr/lib/deadbeef/