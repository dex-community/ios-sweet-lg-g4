#!/bin/bash
# Compatibility wrapper: pack + boot + full read-only battery.
#
#   tools/boot-test-kernel.sh <Image> [evidence.txt]
#
# Kept because older notes call it; all the work is in tools/device-test.sh.
exec "$(cd "$(dirname "$0")" && pwd)/device-test.sh" "$@"
