#!/bin/bash
# Fetch the ramoops console log from the phone and decode the shim's record.
#
# The shim leaves its log in the ramoops console zone of the phone's DRAM.
# The kernel that boots next (the recovery, or Android) reads that zone back
# into /sys/fs/pstore/console-ramoops-0, and this script pulls it and
# prints the record the shim wrote.
#
#   tools/fetch-pstore.sh [outfile]
#
# The raw file is kept next to the output, so that it can be looked at (or
# compared with a later run) even when the record is damaged.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-research/raw-2026-11-05/pstore-console.bin}
mkdir -p "$(dirname "$OUT")"

echo "== pulling /sys/fs/pstore =="
adb shell 'ls -l /sys/fs/pstore/' || true
adb pull /sys/fs/pstore/console-ramoops-0 "$OUT" || {
	echo "no console-ramoops-0: the phone's kernel did not read the zone" >&2
	exit 1
}
echo
python3 tools/read-pstore.py "$OUT"
