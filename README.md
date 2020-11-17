# Sprifd
Control Spotify using RFID tags.

**You need Spotify Premium to use this project!**

## Hardware
* ESP32
* MFRC522
* Lots of rfid tags!

## Howto
* Register an app with Spotify: https://developer.spotify.com/dashboard/applications/8092dd1787fd4fd489cada53ed9f592d
* Add "http://sprfid/callback/" as redirect uri for this app
* Copy Config.tpl to Config.h and copy clientId and clientSecret from Spotify
* Upload to the ESP
* New WiFi access point "sprfid" should appear (Password is "rfidrfid")
* Use the WiFis captive page to connect the device to your own WiFi
* Open http://sprfid/ in your browser
* Click the "spotify Auth" link
* Enjoy your Sprfid player

## Disclaimer
This project is not affiliated with or endorsed by Spotify

## Credits
* Special thanks to [Brian Lough](https://github.com/witnessmenow) for this idea and his awesome libraries
* Thanks to [Don Coleman](https://github.com/don) for his NDEF library
