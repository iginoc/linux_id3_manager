# Music Network Player

A native C++ music player designed to stream audio directly from Samba (SMB) shares without requiring system-level mounting. Built with **GTKmm** for the interface and **GStreamer** for media handling.

## Features

*   **Direct SMB Streaming:** Connects to Windows/Samba shares using `smbclient` to browse and stream music.
*   **Metadata Management:** View and edit ID3 tags (Artist, Title, Album, Year, Genre).
*   **Online Search:** Integrated search using iTunes API to fetch metadata and cover art automatically.
*   **Cover Art:** Displays embedded album art or fetches it from online sources.
*   **File Management:** Rename and delete files/folders directly on the remote share.
*   **Caching:** Local caching of images and metadata for faster browsing.

## Prerequisites

Ensure you have the following installed on your Linux system:

*   **C++ Compiler** with C++17 support (e.g., GCC 8+)
*   **GTKmm 3.0** development libraries
*   **GStreamer 1.0** development libraries and plugins
*   **Runtime Tools:**
    *   `smbclient` (Samba client)
    *   `curl`
    *   `python3` + `pip` (for music recognition)

### Installing Dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential libgtkmm-3.0-dev libgstreamer1.0-dev \
libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good libsqlite3-dev \
gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly smbclient curl \
python3-pip

pip3 install shazamio
```

## Compilation

You can compile the application using `g++`:

```bash
g++ -std=c++17 music_player_setup.cpp -o music_player \
    $(pkg-config --cflags --libs gtkmm-3.0 gstreamer-1.0 gstreamer-tag-1.0) -lsqlite3
```

## Usage

1.  Run the application:
    ```bash
    ./music_player
    ```
2.  On the first run (or via the "Riconfigura" button), enter your Samba details:
    *   **Path:** `//server_address/share_name/folder`
    *   **User:** Your SMB username
    *   **Password:** Your SMB password
3.  Browse folders on the left, select files in the middle, and control playback/metadata on the right.

## Notes

*   The user interface is currently in **Italian**.
*   Configuration is stored in `.music_config` in the application directory.
*   Metadata cache is stored in `.music_metadata_cache` and the `cache/` directory.