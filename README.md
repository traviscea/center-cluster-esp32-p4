# ESP32 Round Display Digital Gauge Cluster

This project is source-available and free for personal and educational use.

**Commercial use is NOT permitted.**

> **Please do not use these files to sell this to others.**
>
> I made these so that those wanting to modify their dash/cluster don't have to spend an arm and a leg to do so.

YouTube Tutorial/Playlist: https://youtu.be/t7H6pevep40

## Getting Started

1. Install [Visual Studio Code](https://code.visualstudio.com/Download).

2. Install the ESP-IDF extension.

   Instructions can be found here:

   https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-3.4C#Introduction_to_ESP-IDF_and_Environment_Setup_.28VSCode_Column.29

   * Open the VSCode Package Manager.
   * Search for the official **ESP-IDF** extension.
   * Install the extension and complete the ESP-IDF setup.

3. Open this repository folder in VSCode.

4. Plug the ESP32-P4/screen into your computer via USB-C.

5. Set the target device to **esp32p4**.

   See the "Description of Bottom Toolbar of VSCode User Interface" section in the Waveshare instructions linked above.

6. Build the project.

7. Flash the project to the ESP32-P4.

<br>

# CANBUS

For CANBUS integration, I have added several CAN protocols such as Haltech, Hondata, etc.

If you would like another CAN protocol added, please join the Discord and provide the CAN protocol/DBC file. I can add support for additional protocols quickly.

<br>

You will need to purchase a [small CAN transceiver](https://a.co/d/09CiRq2o) for approximately $9.

With CANBUS, you can ignore the other sensor wiring if your ECU provides the required sensor data over CAN.

If you need help wiring the CAN transceiver, please join the Discord.

<br>

Also, the sensor source on line 37 of the `main.c` file needs to be changed to:

```c
SENSOR_SOURCE_CAN
```

The default for this code is the analog/non-CAN sensor configuration.

---

# Adding Your Own CAN Protocol

The dash can load CAN protocols from JSON files located in:

```text
main/canbus/protocols/
```

For users who already have a DBC file for their ECU, this repository includes a Python script that can convert a compatible DBC file into the JSON format used by the dash.

The converter is located at:

```text
scripts/dbc_to_json.py
```

## DBC File Location

Your DBC file can be stored anywhere on your computer.

For example, you can create a directory inside the repository:

```text
dbc/
```

and place your DBC files there:

```text
project/
├── dbc/
│   └── my_ecu.dbc
│
├── main/
│   └── canbus/
│       └── protocols/
│
└── scripts/
    └── dbc_to_json.py
```

The DBC file does **not** have to be inside the repository. You can also provide the full path to the file when running the script.

For example:

```bash
python scripts/dbc_to_json.py "/path/to/my_ecu.dbc"
```

---

## DBC to JSON Converter

The converter is a Python 3 script and does not require any additional Python packages.

From the root of the repository, run:

```bash
python scripts/dbc_to_json.py "my_ecu.dbc"
```

For example:

```bash
python scripts/dbc_to_json.py "1_Twinturbozz -06202025_DC - motecDashDesign.dbc"
```

The script will:

1. Read the DBC file.
2. Parse the CAN message definitions (`BO_`).
3. Parse the CAN signals (`SG_`).
4. Convert compatible signals into the dash's JSON format.
5. Automatically determine whether CAN IDs are standard or extended.
6. Convert Intel/little-endian and Motorola/big-endian byte-aligned signals.
7. Write the resulting JSON protocol file to:

```text
main/canbus/protocols/
```

The output filename is based on the DBC filename.

For example:

```text
my_ecu.dbc
```

becomes:

```text
main/canbus/protocols/my_ecu.json
```

### Example Output

When the conversion succeeds, you should see something similar to:

```text
DBC -> Dash JSON Converter
==========================
Input:    /path/to/my_ecu.dbc
Output:   /path/to/project/main/canbus/protocols/my_ecu.json
Name:     my_ecu
Bitrate:  500000

Conversion complete.
Frames:   12
Signals:  47
Extended: 2
Saved:    /path/to/project/main/canbus/protocols/my_ecu.json
```

---

## CAN Bitrate

The default CAN bitrate is:

```text
500000
```

which is **500 kbit/s**.

If your ECU uses a different CAN bitrate, specify it with `--bitrate`.

For example, for 1 Mbit/s:

```bash
python scripts/dbc_to_json.py "my_ecu.dbc" --bitrate 1000000
```

For 250 kbit/s:

```bash
python scripts/dbc_to_json.py "my_ecu.dbc" --bitrate 250000
```

The bitrate is written into the generated JSON file.

---

## Custom Protocol Name

By default, the protocol name is taken from the DBC filename.

For example:

```text
my_ecu.dbc
```

produces:

```json
"name": "my_ecu"
```

You can specify a custom name using `--name`:

```bash
python scripts/dbc_to_json.py "my_ecu.dbc" --name my_ecu
```

---

## Custom Output Location

By default, the generated JSON is placed in:

```text
main/canbus/protocols/
```

You can specify a different output file with `-o` or `--output`:

```bash
python scripts/dbc_to_json.py "my_ecu.dbc" -o "my_ecu.json"
```

For normal dash use, the generated file should ultimately be placed in:

```text
main/canbus/protocols/
```

---

# DBC Signal Compatibility

The dash's JSON CAN protocol format represents signals using **byte offsets and byte lengths**.

Because of this, the converter currently supports **byte-aligned signals**.

For Intel/little-endian signals (`@1`), byte-aligned signals start on:

```text
0, 8, 16, 24, 32, ...
```

For Motorola/big-endian signals (`@0`), byte-aligned signals start on:

```text
7, 15, 23, 31, 39, ...
```

The signal length must also be a whole number of bytes:

```text
8
16
24
32
...
```

### Example

This Motorola signal is supported:

```text
SG_ G_Force_Lat : 7|16@0+ ...
```

It represents a 16-bit signal occupying two complete CAN bytes.

The converter will generate a JSON signal similar to:

```json
{
  "name": "G_Force_Lat",
  "offset": 0,
  "len": 2,
  "scale": 0.01,
  "offset_val": 0,
  "endian": "big"
}
```

The exact scale and offset depend on the values in the DBC file.

### Non-byte-aligned signals

Signals such as:

```text
3|12
```

or:

```text
7|12
```

cannot currently be represented exactly by the dash's JSON format.

The converter will stop with an error rather than generating incorrect CAN decoding information.

If your DBC contains non-byte-aligned signals that you need, please join the Discord and provide the DBC so support can be added to the dash.

---

# Signed Signals

The converter detects signed signals in the DBC.

The current dash JSON protocol format does not contain a dedicated `signed` property, so the converter will display a warning when it encounters one.

For example:

```text
Warning: Signal 'SomeSignal' in frame 'SomeFrame' is signed.
         The target JSON format does not contain a signed field.
```

The resulting JSON is still generated, but signed signal behavior should be verified before relying on that signal.

---

# Multiplexed Signals

The converter detects multiplexed DBC signals and displays a warning.

For example:

```text
Warning: Signal 'SomeSignal' in frame 'SomeFrame' is multiplexed (M).
         Multiplex conditions are not represented in the target JSON format.
```

The current dash JSON format does not represent DBC multiplex conditions.

If your ECU uses multiplexed messages, verify that the resulting protocol behaves correctly with your specific CAN data.

---

# Using the Generated Protocol

After running the converter, copy or leave the generated JSON file in:

```text
main/canbus/protocols/
```

For example:

```text
main/
└── canbus/
    └── protocols/
        ├── haltech.json
        ├── hondata.json
        └── my_ecu.json
```

The protocol can then be selected/used by the dash's CANBUS configuration.

If you add a new protocol to the repository, please consider contributing the protocol back to the project so other users with the same ECU can use it.

<br>

# Issues / Bug Fixes

## 4in Waveshare Round Screens

For 4in round screens, the base tach image needs to be updated as the resolution is **720x720** and not **800x800** like the 3.4in screens.

<br>

To do that, replace the:

```text
ui_img_1656279599.c
```

file in:

```text
main/tach_ui/images/
```

with the file found here:

https://drive.google.com/file/d/1_PrP6jOna2s5Ua82ol2qqV2OZWf9c4_l/view?usp=sharing

<br>

## Older Boards / Revision Errors

If you see an error like:

```text
A fatal error occurred: bootloader/bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3).
```

you need to update the minimum supported board revision with a few clicks.

Follow the instructions here:

https://discord.com/channels/1474501462905192450/1474502475280154927/1480043568469901459

```

I kept the instructions beginner-friendly since this is an **open-source DIY dash project**, and made the DBC section explicit enough that someone can clone the repo, drop in a DBC, run one command, and know exactly where the resulting JSON goes.
```
