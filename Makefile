CXX = g++
CXXFLAGS = -std=c++17 -Wall $(shell pkg-config gtkmm-3.0 gstreamer-1.0 gstreamer-tag-1.0 --cflags)
LDFLAGS = $(shell pkg-config gtkmm-3.0 gstreamer-1.0 gstreamer-tag-1.0 --libs)
LDFLAGS += -lsqlite3 -lasound
SRC = music_player_setup.cpp
TARGET = music_player

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
