# SndVolHWMixer

A real mixer board-styled volume control for Windows.

What makes this application special is the ability to retreive active streams from the audio endpoint device and send them to the hardware.
The end goal is to make it as practical as the Windows SndVol application.

The Windows application is built in VC++ with VS2017.

The Arduino end is built in a Arduino Mega2560
Use either the Arduino IDE or Platform.IO.
The Arduino program requires the Adafruit GFX library and the Adafruit SSD1306 library.

If using anything else than the Arduino IDE, get the libraries by using git submodule:

git submodule init

git submodule update --remote 