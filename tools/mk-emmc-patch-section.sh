#!/bin/bash
# Rebuild the bsd/dev/sweet_emmc.c section of xnu/patches/0000-sweet-source-files.patch
# from a source file.
#
#   tools/mk-emmc-patch-section.sh <source.c>
#
# The section is a "new file" diff, so the source is prefixed with '+' and the
# hunk header counts the added lines. The blob hash is the real git one, so
# the patch stays consistent with the file it carries.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${1:?usage: mk-emmc-patch-section.sh <source.c>}
PATCH=xnu/patches/0000-sweet-source-files.patch

[ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "no such file: $PATCH" >&2; exit 1; }

lines=$(wc -l < "$SRC" | tr -d ' ')
hash=$(git hash-object "$SRC")

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

{
	echo "diff --git a/bsd/dev/sweet_emmc.c b/bsd/dev/sweet_emmc.c"
	echo "new file mode 100644"
	echo "index 0000000..$hash"
	echo "--- /dev/null"
	echo "+++ b/bsd/dev/sweet_emmc.c"
	echo "@@ -0,0 +1,$lines @@"
	sed 's/^/+/' "$SRC"
} > "$tmp"

# Replace the old section (from the first diff --git to the one that follows).
awk -v newfile="$tmp" '
	/^diff --git a\/drivers\/NativeSdhci\/BoardConstants.h/ && !done {
		while ((getline line < newfile) > 0) print line
		done = 1
	}
	{ if (!done) next; print }
' "$PATCH" > "$tmp.out"

mv "$tmp.out" "$PATCH"
echo "bsd/dev/sweet_emmc.c: $lines lines, blob $hash"
