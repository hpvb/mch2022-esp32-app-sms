# MCH2022 Master System emulator

This repo contains a master system emulator for the badge. It doesn't currently have a menu or SD card driver, you have to include a game with the build.

# Features

* It runs SEGA Master System games at around 50FPS

# How to build
Add a game as `main/rom.sms`

```
git clone --recursive https://github.com/hpvb/mch2022-esp32-app-sms
cd mch2022-esp32-app-sms
make
```

This is based on the https://github.com/badgeteam/mch2022-template-app, you can view the documentation there

# Thanks

* The excellent TotalSMS emulator https://github.com/ITotalJustice/TotalSMS
* The excellent people in the badge tent for helping me work out the ESP-IDF stuff
* SEGA? sure why not

