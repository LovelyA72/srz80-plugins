#!/usr/bin/env python3
"""Your ROM not working? This might tell you the reason"""
import hashlib
from pathlib import Path
import sys

ROMS = {
    "xr174c0.ic7": (524288, "9ca892920598f9fdf08544dac4c0e54e7d46ee3c"),
    "xq057c0.ic18": (2097152, "32f653c7644d060f5a6d63a435ae3a7412386d92"),
    "xq058c0.ic19": (2097152, "adf68689b4842ec5bc9b0ea1bb99cf66d2dec4de"),
}

def main():
    directory = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parent
    if len(sys.argv) > 2:
        sys.exit("Usage: verify_roms.py [ROM directory]")
    for name, (size, sha1) in ROMS.items():
        try:
            data = (directory / name).read_bytes()
        except OSError as exc:
            sys.exit(str(exc))
        if len(data) != size or hashlib.sha1(data).hexdigest() != sha1:
            sys.exit(f"{name}: wrong ROM; expected {size} bytes, SHA1 {sha1}")
        print(f"{name}: verified")

if __name__ == "__main__":
    main()
