# MCH2022 Master System emulator

This repo contains a master system emulator for the badge. It doesn't currently have a menu or SD card driver, you have to include a game with the build.

# Features

* It runs SEGA Master System games 
* CPU runs at 100% speed for all games I've tested
* Frame updates around 60 FPS so transparency effects work
* Audio support
* The border of the screen is the correct overscan color
* Pause button! (select)

# TODO

* Load/save emulator state
* Add a menu for settings
* Add a cheat menu
* Add a ROM loader menu
* Add a way to download roms from the internet

# How to build

```
git clone --recursive https://github.com/hpvb/mch2022-esp32-app-sms
cd mch2022-esp32-app-sms
# Add a game as `main/rom.sms`
make
```

This is based on the https://github.com/badgeteam/mch2022-template-app, you can view the documentation there

# Thanks

* The excellent TotalSMS emulator https://github.com/ITotalJustice/TotalSMS
* The generous people in the badge tent for helping me work out the ESP-IDF stuff
* The patient and kind people in the badge.team Telegram
* Sylvain Munaut for the FPGA bitstream to achieve 60FPS
* SEGA? sure why not

