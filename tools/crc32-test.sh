#!/bin/bash
# crc32-test.sh -- the shim's CRC32 must be the same as the host's.
#
# The shim verifies the payload it reads from the microSD; the host writes
# it. If the two CRCs differ, the shim would refuse a good payload (or
# accept a bad one). This test compares them over random buffers.
set -euo pipefail
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/t.c" <<'CEOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shim/crc32.h"
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	long n;
	uint8_t *buf;

	if (!f) return 2;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n ? (size_t)n : 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 3;
	printf("%08x\n", crc32_le(0xffffffffu, buf, (uint32_t)n) ^ 0xffffffffu);
	return 0;
}
CEOF

gcc -O2 -Wall -o "$TMP/t" "$TMP/t.c" shim/crc32.c -I.
rc=0
for i in 1 2 3 4 5 6 7 8 9 10; do
	head -c $((RANDOM * 37 + 1)) /dev/urandom > "$TMP/buf"
	ours=$("$TMP/t" "$TMP/buf")
	theirs=$(python3 -c "import zlib,sys; print('%08x' % zlib.crc32(open(sys.argv[1],'rb').read()))" "$TMP/buf")
	if [ "$ours" != "$theirs" ]; then
		echo "FAIL: size $(stat -c%s "$TMP/buf" 2>/dev/null || stat -f%z "$TMP/buf"): shim=$ours zlib=$theirs"
		rc=1
	fi
done
[ $rc -eq 0 ] && echo "OK: the shim's CRC32 is the same as zlib's"
exit $rc
