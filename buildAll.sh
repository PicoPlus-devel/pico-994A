:
# ====================================================================================
# pico-994A build all script
#
# Builds the TI-99/4A emulator for every RP2350 hardware configuration.
# Binaries are copied to the releases folder.
#
# HW_CONFIG 3 (Adafruit Feather RP2040 DVI) and 4 (Waveshare RP2040-PiZero) are
# RP2040-only boards and 11 is a deprecated pinout, so they are not built here.
# ====================================================================================
cd `dirname $0` || exit 1
[ -d releases ] && rm -rf releases
mkdir releases || exit 1
# check picotool exists in path
if ! command -v picotool &> /dev/null
then
	echo "picotool could not be found"
	echo "Please install picotool from https://github.com/raspberrypi/picotool.git"
	exit 1
fi
# build for Pico 2 -arm-s
#   1  Pimoroni Pico DV Demo Base       2  Adafruit DVI + microSD breakout / custom PCB
#   5  Adafruit Metro RP2350            6  Waveshare RP2350-Zero with custom PCB
#   7  Waveshare RP2350-PiZero          8  Adafruit Fruit Jam (default)
#   9  Waveshare RP2350-USB-A          10  Spotpear HDMI board
#  12  Murmulator M1                   13  Murmulator M2
#  14  Adafruit Feather RP2350
HWCONFIGS="1 2 5 6 7 8 9 10 12 13 14"
for HWCONFIG in $HWCONFIGS
do
	./bld.sh -c $HWCONFIG -2 || exit 1
done
if [ -z "$(ls -A releases)" ]; then
	echo "No UF2 files found in releases folder"
	exit 1
fi
for UF2 in releases/*.uf2
do
	ls -l $UF2
	picotool info $UF2
	echo " "
done
