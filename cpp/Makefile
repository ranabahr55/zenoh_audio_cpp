# Build the C++ audio recorder.
#
#   make        # build ./record_audio
#   make run    # build and record
#   make clean  # remove build artefacts

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PKG      := portaudio-2.0

CXXFLAGS += $(shell pkg-config --cflags $(PKG))
LDLIBS   += $(shell pkg-config --libs $(PKG))

TARGET := record_audio
SRC    := record_audio.cpp

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

.PHONY: run clean
run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) recording_raw.wav
