#!/usr/bin/env python3
"""Read the shim's record out of a ramoops zone.

The shim leaves its log in the console zone of the ramoops region, in the
format the kernel's pstore driver reads back:

    [G4SHIMPS1] len=<n> crc=<crc32>\n
    <the log>
    [G4SHIMPS1-END]\n

The CRC is what tells a record that survived the trip from the phone to the
host from one that the Reed-Solomon decoder mangled on the way.

WHICH FILE
----------
The shim writes the record in THREE places (shim/pstore.c):

  console zone, 0x0ff80000   what the recovery reads as console-ramoops-0
  console zone, 0x0ff00000   the other console zone (Android's layout)
  pmsg zone,   0x0ff20000   what the recovery reads as pmsg-ramoops-0

The console zones are EATEN by the kernel: the recovery writes its own
console there from the moment it boots, and the zone is a ring, so the
record of a run that finished is gone by then. That is why the pmsg zone
matters: no kernel writes its console there (the console goes to the
console zone), so the record survives and comes back as
/sys/fs/pstore/pmsg-ramoops-0.

So, when reading the log of a run, pull the pmsg file FIRST:

    adb pull /sys/fs/pstore/pmsg-ramoops-0 pmsg.bin
    tools/read-pstore.py pmsg.bin

The file can be

  - /sys/fs/pstore/pmsg-ramoops-0 (or console-ramoops-0) pulled from the
    phone: the kernel has already rebuilt the ring into it, or
  - a raw dump of the zone (the first 12 bytes are the zone header: the
    signature, the start and the size of the ring).

Both are searched for the marker, so it does not matter which one it is.

    tools/read-pstore.py console-ramoops-0.bin
    adb pull /sys/fs/pstore/pmsg-ramoops-0 pmsg.bin && tools/read-pstore.py pmsg.bin
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
