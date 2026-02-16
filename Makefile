CXX = g++
CXXFLAGS = -std=c++17 -Wall $(shell pkg-config gtkmm-3.0 gstreamer-1.0 --cflags)
LDFLAGS = $(shell pkg-config gtkmm-3.0 gstreamer-1.0 --libs)
TARGET = music_player
SRC = music_player_setup.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
