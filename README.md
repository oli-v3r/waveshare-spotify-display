# Waveshare Spotify Display

Firmware for a Waveshare ESP32-S3 AMOLED display. It connects to Wi-Fi and
Spotify, checks the currently playing track, downloads its album artwork, and
uses LVGL to display the image.

## Project status

This project is being brought back into working order. The display and JPEG
decoder have been tested with one built-in JPEG image. Live Spotify artwork
downloads are still being debugged.

## Getting started

1. Install PlatformIO and connect the ESP32-S3 board.
2. Create your local credentials file:

   ```sh
   cp include/secrets.h.example include/secrets.h
   ```

   Add your Wi-Fi and Spotify credentials to `include/secrets.h`. This file is
   ignored by Git; never commit or share it.
3. Build and upload the firmware:

   ```sh
   pio run
   pio run -t upload
   ```

   If needed, open the serial monitor at 115200 baud:

   ```sh
   pio device monitor
   ```

## Project layout

- `src/main.cpp` — display, Spotify polling, and album-art download logic.
- `src/test_img.h` — the single built-in JPEG used to test the display and
  decoder independently of Wi-Fi and Spotify.
- `include/` — local credentials template and project headers.
- `platformio.ini` — board, libraries, and build configuration.
- `tools/` — helper for authorizing the Spotify app and setting up a refresh
  token.

The test image currently appears at startup. It confirms the display path
works, but is not proof that live Spotify artwork has downloaded successfully.
