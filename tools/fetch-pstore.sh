#!/bin/bash
# Fetch the ramoops log from the phone and decode the shim's record.
#
# The shim leaves its log in the ramoops region of the phone's DRAM. The
# kernel that boots next (the recovery, or Android) reads that region back
# into /sys/fs/pstore/, one file per zone:
#
#   pmsg-ramoops-0      THE ONE THAT SURVIVES.  The kernel never writes its
#                       console there, so the record of the run that just
#                       happened is still intact.
#   console-ramoops-0   the kernel's own console.  It EATS the record the
#                       shim wrote in the console zone, because the kernel
#                       writes its console there from the moment it boots
#                       and the zone is a ring.
#
# So this script pulls the pmsg file FIRST and decodes that one; the console
# file is pulled too, and decoded as well, but what it shows is normally the
# kernel's log rather than the shim's.
#
#   tools/fetch-pstore.sh [outfile]
#
# The raw files are kept next to the output, so that they can be looked at
# (or compared with a later run) even when a record is damaged.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-research/raw-2026-11-05/pstore-console.bin}
mkdir -p "$(dirname "$OUT")"

PMSG="${OUT%.bin}-pmsg.bin"
CONSOLE="${OUT%.bin}-console.bin"

echo "== pulling /sys/fs/pstore =="
adb shell 'ls -l /sys/fs/pstore/' || true

# The pmsg zone first: it is the copy that the kernel does not overwrite.
if adb pull /sys/fs/pstore/pmsg-ramoops-0 "$PMSG" 2>/dev/null; then
	echo
	echo "== the pmsg zone (the shim's record, if the shim wrote it) =="
	python3 tools/read-pstore.py "$PMSG" || true
else
	echo "no pmsg-ramoops-0: the phone's kernel did not read that zone" >&2
fi

echo
echo "== the console zone (the kernel's console, usually) =="
if adb pull /sys/fs/pstore/console-ramoops-0 "$CONSOLE" 2>/dev/null; then
	python3 tools/read-pstore.py "$CONSOLE" || true
else
	echo "no console-ramoops-0: the phone's kernel did not read that zone" >&2
	exit 1
fi
