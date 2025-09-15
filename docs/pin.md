# Connector and Pin Header Description

## J1 USB-C Connector

This terminal is for uploading firmware and checking program logs.
It is recognized as a serial device and as a `ttyACM` device in Linux.
To view the logs, you can use the command below.

```bash
minicom -D /dev/ttyACM0 -b 115200
```

## J2 UART Pin header

Connected to the `U0RXD`, `U0TXD` system UART of the ESP32-C3. Currently not used on this board.

## J3 ODROID UART Connector

Connects to the UART debug port of the ODROID.
This connector is `X8821WRS-04`. you can use the [cable from the USB-UART 2 board](https://www.hardkernel.com/shop/usb-uart-2-module-kit-copy/).

## J4 GPIO Output

4 GPIO output pin headers. Used to trigger the power of external devices.

| Pin Number | Function           | Function           | Pin Number |
|------------|--------------------|--------------------|------------|
| 3          | Reserved           | Reserved           | 1          |
| 4          | Reset (Open Drain) | Power (Open Drain) | 2          |

The Reset and Power pins can be used on the ODROID-M series and H series.

## J5 DC Input

Supports 9~21v input. You can use the DC power supply sold by Hardkernel.

## J6 DC Out

Outputs the switched power from the J5 input.

## J7 USB Power Out

Switched 5v voltage output.
The output is set to 5.25V considering voltage drop. Please use with caution.

## J8 OLED Display Connector

**Reserved**

SSD1309 OLED Connector