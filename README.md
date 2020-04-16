# RPi SSB Transceiver

An RPi4 + 7" Touchscreen-based SSB Transceiver. Work in Progress.

Large portions of this project are heavily derived from the work of other open-source projects, eg. [github.com/ha7ilm/csdr](https://github.com/ha7ilm/csdr/).

![Example Screenshot](https://raw.githubusercontent.com/philcrump/rpi-txrx/master/screenshot.png)

## Installation

sudo apt install libfftw3-dev libasound2-dev

`sudo cp limesdr-mini.rules /etc/udev/rules.d/`

/boot/config.txt
```
disable_splash=1
```

## Fonts

Download a .TTF and then process into bitmap arrays with:

`./process.py <font file> <font size> <font variable name>`

eg. `./process.py DejaVuSans.ttf 36 dejavu_sans > dejavu_sans_36.c`