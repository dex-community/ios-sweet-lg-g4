#!/usr/bin/env python3
"""Read the shim's record out of the ramoops console zone.

The shim leaves its log in the console zone of the ramoops region, in the
format the kernel's pstore driver reads back:

    [G4SHIMPS1] len=<n> crc=<crc32>\n
    <the log>
    [G4SHIMPS1-END]\n

The CRC is what tells a record that survived the trip from the phone to the
host from one that the Reed-Solomon decoder mangled on the way.

The file can be

  - /sys/fs/pstore/console-ramoops-0 pulled from the phone (the kernel has
    already rebuilt the ring into it), or
  - a raw dump of the console zone (the first 12 bytes are the zone header:
    the signature, the start and the size of the ring).

Both are searched for the marker, so it does not matter which one it is.

    tools/read-pstore.py console-ramoops-0.bin
    adb pull /sys/fs/pstore/console-ramoops-0 pstore.bin && tools/read-pstore.py pstore.bin
"""
import re
import sys
import zlib

MAGIC = b"[G4SHIMPS1] len="
END = b"[G4SHIMPS1-END]"


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        data = f.read()

    i = data.rfind(MAGIC)
    if i < 0:
        print("no shim record in %s (%d bytes)" % (argv[1], len(data)))
        return 1

    end_of_header = data.find(b"\n", i)
    if end_of_header < 0:
        print("the record header is truncated")
        return 1
    header = data[i:end_of_header].decode("ascii", "replace")
    m = re.search(r"len=(\d+) crc=([0-9a-fA-F]+)", header)
    if not m:
        print("the record header is not understood: %r" % header)
        return 1

    length = int(m.group(1))
    crc = int(m.group(2), 16)
    payload = data[end_of_header + 1:end_of_header + 1 + length]
    good = len(payload) == length and (zlib.crc32(payload) & 0xFFFFFFFF) == crc
    # The end marker is written after the payload, not inside it.
    after = data[end_of_header + 1 + length:end_of_header + 1 + length + len(END)]
    complete = after == END

    print("record at offset %d: %s" % (i, header))
    print("payload: %d bytes, crc %s, end marker %s" %
          (len(payload), "ok" if good else "WRONG",
           "present" if complete else "MISSING"))
    if not good:
        print("the record is damaged: the parity blocks did not survive")
        return 1
    sys.stdout.write(payload.decode("ascii", "replace"))
    if not payload.endswith(b"\n"):
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
