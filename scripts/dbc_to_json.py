#!/usr/bin/env python3

"""
DBC -> CAN Dash JSON Converter

Project structure expected:

    project/
    ├── main/
    │   └── canbus/
    │       └── protocols/
    │           ├── haltech.json
    │           ├── hondata.json
    │           └── ...
    │
    └── scripts/
        └── dbc_to_json.py

Usage:

    python scripts/dbc_to_json.py protocol.dbc

    python scripts/dbc_to_json.py protocol.dbc --bitrate 1000000

    python scripts/dbc_to_json.py protocol.dbc -o custom.json

    python scripts/dbc_to_json.py protocol.dbc --name emtron

Default bitrate:

    500000

The protocol name defaults to the DBC filename without its extension.

The default output directory is:

    ../main/canbus/protocols/

relative to this script.
"""

import argparse
import json
import re
import sys
from pathlib import Path


# ============================================================================
# Configuration
# ============================================================================

DEFAULT_BITRATE = 500000


# ============================================================================
# Numeric helpers
# ============================================================================

def parse_number(value):
    """
    Convert a DBC numeric string into an int or float.

    Examples:
        "1"       -> 1
        "1.0"     -> 1
        "0.001"   -> 0.001
        "-50"     -> -50
    """

    value = value.strip()

    try:
        number = float(value)
    except ValueError:
        raise ValueError(f"Invalid numeric value: {value}")

    # Keep whole numbers as integers so the resulting JSON is cleaner.
    if number.is_integer():
        return int(number)

    return number


# ============================================================================
# CAN ID helpers
# ============================================================================

def dbc_id_to_can_id(raw_id):
    """
    Convert the ID stored in a DBC BO_ line into the actual CAN ID.

    DBC files commonly use bit 31 as the extended-frame marker.

    Standard CAN:
        11-bit ID

    Extended CAN:
        29-bit ID with bit 31 used as the DBC extended-frame marker.

    Returns:
        actual_can_id, is_extended
    """

    raw_id = int(raw_id)

    # Bit 31 is commonly used by DBC files to indicate an extended frame.
    if raw_id & 0x80000000:
        return raw_id & 0x1FFFFFFF, True

    return raw_id, False


def format_hex_id(can_id):
    """Return a CAN ID in the desired hexadecimal format."""

    return f"0x{can_id:X}"


# ============================================================================
# Signal conversion
# ============================================================================

def convert_signal(
    signal_name,
    start_bit,
    length,
    byte_order,
    signed,
    factor,
    offset,
    frame_name,
):
    """
    Convert one DBC signal into the dash JSON representation.

    Target format:

        {
          "name": "...",
          "offset": 0,
          "len": 2,
          "scale": 1.0,
          "offset_val": 0,
          "endian": "little"
        }

    The target JSON format uses byte offsets and byte lengths.

    Therefore, arbitrary bit-level signals cannot be represented exactly.

    IMPORTANT:

    Intel / little-endian DBC signals:
        The start bit is the least-significant bit.

        Byte-aligned signals therefore start at:
            0, 8, 16, 24, ...

    Motorola / big-endian DBC signals:
        The start bit identifies the most-significant bit.

        Byte-aligned signals therefore start at:
            7, 15, 23, 31, ...

    Example Motorola signal:

        SG_ G_Force_Lat : 7|16@0+ ...

        This occupies:
            byte 0
            byte 1

        JSON representation:

            offset = 0
            len    = 2
            endian = "big"
    """

    start_bit = int(start_bit)
    length = int(length)

    if length <= 0:
        raise ValueError(
            f"Signal '{signal_name}' in frame '{frame_name}' "
            f"has invalid length {length}"
        )

    # ------------------------------------------------------------------------
    # Length must always be an exact number of bytes.
    # ------------------------------------------------------------------------

    if length % 8 != 0:
        raise ValueError(
            f"Signal '{signal_name}' in frame '{frame_name}' "
            f"is not byte-aligned "
            f"(start={start_bit}, length={length}). "
            f"The target JSON format requires byte-aligned signals."
        )

    byte_length = length // 8

    # ------------------------------------------------------------------------
    # Intel / Little Endian
    # ------------------------------------------------------------------------

    if byte_order == "1":

        # For Intel signals, the start bit must be the first bit
        # of a byte for the target JSON representation.
        #
        # Valid starts:
        #
        #     0
        #     8
        #     16
        #     24
        #     ...

        if start_bit % 8 != 0:
            raise ValueError(
                f"Intel signal '{signal_name}' in frame "
                f"'{frame_name}' is not byte-aligned "
                f"(start={start_bit}, length={length}). "
                f"Intel byte-aligned signals must start on "
                f"bit 0, 8, 16, 24, etc."
            )

        byte_offset = start_bit // 8
        endian = "little"

    # ------------------------------------------------------------------------
    # Motorola / Big Endian
    # ------------------------------------------------------------------------

    elif byte_order == "0":

        # In a Motorola DBC signal, start_bit identifies the MSB.
        #
        # Therefore a byte-aligned Motorola signal starts on:
        #
        #     bit 7
        #     bit 15
        #     bit 23
        #     bit 31
        #     ...
        #
        # Example:
        #
        #     start_bit = 7
        #     length    = 16
        #
        #     Byte 0        Byte 1
        #     [7.......0]  [7.......0]
        #      ^ MSB          ^ LSB
        #
        #     JSON offset = 0
        #     JSON len    = 2

        if start_bit % 8 != 7:
            raise ValueError(
                f"Motorola signal '{signal_name}' in frame "
                f"'{frame_name}' is not byte-aligned "
                f"(start={start_bit}, length={length}). "
                f"Motorola byte-aligned signals must start on "
                f"bit 7, 15, 23, 31, etc."
            )

        # Since start_bit is the MSB:
        #
        #     bit 7  -> byte 0
        #     bit 15 -> byte 1
        #     bit 23 -> byte 2
        #
        byte_offset = (start_bit - 7) // 8
        endian = "big"

    else:

        raise ValueError(
            f"Unknown byte order '@{byte_order}' for "
            f"signal '{signal_name}'"
        )

    # ------------------------------------------------------------------------
    # Create target signal
    # ------------------------------------------------------------------------

    signal = {
        "name": signal_name,
        "offset": byte_offset,
        "len": byte_length,
        "scale": factor,
        "offset_val": offset,
        "endian": endian,
    }

    return signal, signed


# ============================================================================
# DBC parser
# ============================================================================

def parse_dbc(dbc_path):
    """
    Parse a DBC file.

    Returns:
        A list of frames.
    """

    frames = []
    current_frame = None

    # ------------------------------------------------------------------------
    # BO_ message definition
    #
    # Example:
    #
    # BO_ 1250 EngineData: 8 ECU
    # ------------------------------------------------------------------------

    frame_pattern = re.compile(
        r"^BO_\s+"
        r"(\d+)\s+"
        r"([^:]+)"
        r":\s*"
        r"(\d+)\s+"
        r"(.+)$"
    )

    # ------------------------------------------------------------------------
    # SG_ signal definition
    #
    # Example:
    #
    # SG_ RPM : 0|16@1+ (1,0) [0|8000] "rpm" ECU
    #
    # Groups:
    #
    #   1 = signal name
    #   2 = multiplexer indicator
    #   3 = start bit
    #   4 = signal length
    #   5 = byte order
    #   6 = signedness
    #   7 = factor
    #   8 = offset
    #
    # This pattern intentionally stops after the factor/offset because
    # everything after that is not needed for the target JSON.
    # ------------------------------------------------------------------------

    signal_pattern = re.compile(
        r"^SG_\s+"
        r"([^\s:]+)"
        r"(?:\s+([Mm][0-9]+|M))?"
        r"\s*:\s*"
        r"(\d+)\|(\d+)"
        r"@([01])([+-])"
        r"\s*"
        r"\(\s*"
        r"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)"
        r"\s*,\s*"
        r"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)"
        r"\s*\)"
    )

    # ------------------------------------------------------------------------
    # Read DBC
    # ------------------------------------------------------------------------

    with open(
        dbc_path,
        "r",
        encoding="utf-8-sig",
        errors="replace",
    ) as file:

        for line_number, raw_line in enumerate(file, 1):

            line = raw_line.strip()

            if not line:
                continue

            # =================================================================
            # Frame
            # =================================================================

            frame_match = frame_pattern.match(line)

            if frame_match:

                raw_id = int(frame_match.group(1))
                frame_name = frame_match.group(2).strip()
                dlc = int(frame_match.group(3))

                can_id, extended = dbc_id_to_can_id(raw_id)

                current_frame = {
                    "id": format_hex_id(can_id),
                    "signals": [],

                    # Internal metadata.
                    # These are removed before writing JSON.

                    "_name": frame_name,
                    "_dlc": dlc,
                    "_extended": extended,
                    "_raw_id": raw_id,
                }

                frames.append(current_frame)

                continue

            # =================================================================
            # Signal
            # =================================================================

            if line.startswith("SG_"):

                if current_frame is None:

                    print(
                        f"Warning: SG_ found before BO_ "
                        f"at line {line_number}; skipping."
                    )

                    continue

                signal_match = signal_pattern.match(line)

                if not signal_match:

                    print(
                        f"Warning: Could not parse signal at line "
                        f"{line_number}:"
                    )

                    print(f"    {line}")

                    continue

                signal_name = signal_match.group(1)

                # Multiplexer information.
                multiplex = signal_match.group(2)

                start_bit = int(signal_match.group(3))
                length = int(signal_match.group(4))

                byte_order = signal_match.group(5)
                sign = signal_match.group(6)

                factor = parse_number(signal_match.group(7))
                offset = parse_number(signal_match.group(8))

                signed = sign == "-"

                # -------------------------------------------------------------
                # Convert signal
                # -------------------------------------------------------------

                try:

                    signal, signed = convert_signal(
                        signal_name=signal_name,
                        start_bit=start_bit,
                        length=length,
                        byte_order=byte_order,
                        signed=signed,
                        factor=factor,
                        offset=offset,
                        frame_name=current_frame["_name"],
                    )

                except ValueError as exc:

                    raise ValueError(
                        f"Line {line_number}: {exc}"
                    ) from exc

                # -------------------------------------------------------------
                # Multiplex warning
                # -------------------------------------------------------------

                if multiplex:

                    print(
                        f"Warning: Signal '{signal_name}' in "
                        f"frame '{current_frame['_name']}' is multiplexed "
                        f"({multiplex})."
                    )

                    print(
                        "         Multiplex conditions are not represented "
                        "in the target JSON format."
                    )

                # -------------------------------------------------------------
                # Signed signal warning
                # -------------------------------------------------------------

                if signed:

                    print(
                        f"Warning: Signal '{signal_name}' in "
                        f"frame '{current_frame['_name']}' is signed."
                    )

                    print(
                        "         The target JSON format does not contain "
                        "a signed field."
                    )

                current_frame["signals"].append(signal)

    return frames


# ============================================================================
# JSON output
# ============================================================================

def write_json(
    output_path,
    protocol_name,
    bitrate,
    frames,
):
    """
    Write the converted protocol JSON.

    Signal objects are deliberately kept on one line.
    """

    with open(
        output_path,
        "w",
        encoding="utf-8",
    ) as file:

        file.write("{\n")

        # --------------------------------------------------------------------
        # Protocol name
        # --------------------------------------------------------------------

        file.write(
            f'  "name": {json.dumps(protocol_name)},\n'
        )

        # --------------------------------------------------------------------
        # Bitrate
        # --------------------------------------------------------------------

        file.write(
            f'  "bitrate": {bitrate},\n'
        )

        # --------------------------------------------------------------------
        # Frames
        # --------------------------------------------------------------------

        file.write(
            '  "frames": [\n'
        )

        for frame_index, frame in enumerate(frames):

            file.write("    {\n")

            file.write(
                f'      "id": {json.dumps(frame["id"])},\n'
            )

            file.write(
                '      "signals": [\n'
            )

            # ---------------------------------------------------------------
            # Signals
            # ---------------------------------------------------------------

            for signal_index, signal in enumerate(
                frame["signals"]
            ):

                signal_json = json.dumps(
                    signal,
                    separators=(",", ":"),
                    ensure_ascii=False,
                )

                if signal_index < len(frame["signals"]) - 1:
                    signal_json += ","

                file.write(
                    f"        {signal_json}\n"
                )

            file.write(
                "      ]\n"
            )

            # ---------------------------------------------------------------
            # Frame comma
            # ---------------------------------------------------------------

            if frame_index < len(frames) - 1:
                file.write("    },\n")
            else:
                file.write("    }\n")

        file.write(
            "  ]\n"
        )

        file.write(
            "}\n"
        )


# ============================================================================
# Command line
# ============================================================================

def main():

    parser = argparse.ArgumentParser(
        description=(
            "Convert a CAN DBC file to the dash CAN protocol JSON format."
        )
    )

    # ------------------------------------------------------------------------
    # Input DBC
    # ------------------------------------------------------------------------

    parser.add_argument(
        "input",
        help="Input DBC file",
    )

    # ------------------------------------------------------------------------
    # Output
    # ------------------------------------------------------------------------

    parser.add_argument(
        "-o",
        "--output",
        default=None,
        help=(
            "Output JSON file. "
            "If omitted, output is written to "
            "main/canbus/protocols/."
        ),
    )

    # ------------------------------------------------------------------------
    # Bitrate
    # ------------------------------------------------------------------------

    parser.add_argument(
        "--bitrate",
        type=int,
        default=DEFAULT_BITRATE,
        help=(
            f"CAN bitrate in bits/sec "
            f"(default: {DEFAULT_BITRATE})"
        ),
    )

    # ------------------------------------------------------------------------
    # Protocol name
    # ------------------------------------------------------------------------

    parser.add_argument(
        "--name",
        default=None,
        help=(
            "Protocol name. "
            "Defaults to the DBC filename without extension."
        ),
    )

    args = parser.parse_args()

    # =========================================================================
    # Input validation
    # =========================================================================

    input_path = Path(args.input)

    if not input_path.exists():

        print(
            f"Error: Input file does not exist: {input_path}",
            file=sys.stderr,
        )

        return 1

    if not input_path.is_file():

        print(
            f"Error: Input path is not a file: {input_path}",
            file=sys.stderr,
        )

        return 1

    if input_path.suffix.lower() != ".dbc":

        print(
            f"Warning: Input file does not have a .dbc extension: "
            f"{input_path}"
        )

    # =========================================================================
    # Protocol name
    # =========================================================================

    if args.name:
        protocol_name = args.name
    else:
        protocol_name = input_path.stem

    # =========================================================================
    # Output path
    # =========================================================================

    if args.output:

        # User explicitly specified output.
        output_path = Path(args.output)

    else:

        # ---------------------------------------------------------------------
        # Determine project root.
        #
        # Project structure:
        #
        # project/
        # ├── main/
        # │   └── canbus/
        # │       └── protocols/
        # │
        # └── scripts/
        #     └── dbc_to_json.py
        #
        # __file__ = project/scripts/dbc_to_json.py
        #
        # parent       = project/scripts
        # parent.parent = project
        # ---------------------------------------------------------------------

        script_folder = Path(__file__).resolve().parent
        project_root = script_folder.parent

        output_folder = (
            project_root
            / "main"
            / "canbus"
            / "protocols"
        )

        # Create the output folder if necessary.
        output_folder.mkdir(
            parents=True,
            exist_ok=True,
        )

        output_path = (
            output_folder
            / f"{input_path.stem}.json"
        )

    # =========================================================================
    # Validate bitrate
    # =========================================================================

    if args.bitrate <= 0:

        print(
            "Error: Bitrate must be greater than zero.",
            file=sys.stderr,
        )

        return 1

    # =========================================================================
    # Display settings
    # =========================================================================

    print()
    print("DBC -> Dash JSON Converter")
    print("==========================")
    print(f"Input:    {input_path.resolve()}")
    print(f"Output:   {output_path.resolve()}")
    print(f"Name:     {protocol_name}")
    print(f"Bitrate:  {args.bitrate}")
    print()

    # =========================================================================
    # Parse DBC
    # =========================================================================

    try:

        frames = parse_dbc(input_path)

    except Exception as exc:

        print(
            "Error while parsing DBC:",
            file=sys.stderr,
        )

        print(
            f"  {exc}",
            file=sys.stderr,
        )

        return 1

    # =========================================================================
    # Write JSON
    # =========================================================================

    try:

        write_json(
            output_path=output_path,
            protocol_name=protocol_name,
            bitrate=args.bitrate,
            frames=frames,
        )

    except OSError as exc:

        print(
            "Error writing output file:",
            file=sys.stderr,
        )

        print(
            f"  {exc}",
            file=sys.stderr,
        )

        return 1

    # =========================================================================
    # Statistics
    # =========================================================================

    signal_count = sum(
        len(frame["signals"])
        for frame in frames
    )

    extended_count = sum(
        1
        for frame in frames
        if frame["_extended"]
    )

    # =========================================================================
    # Done
    # =========================================================================

    print("Conversion complete.")
    print(f"Frames:   {len(frames)}")
    print(f"Signals:  {signal_count}")
    print(f"Extended: {extended_count}")
    print(f"Saved:    {output_path.resolve()}")
    print()

    return 0


# ============================================================================
# Entry point
# ============================================================================

if __name__ == "__main__":
    sys.exit(main())