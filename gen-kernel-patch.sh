#!/bin/bash
# gen-kernel-patch.sh - generate a kernel patch that integrates ssv6xxx-ng
# into a kernel source tree (drivers/net/wireless/ssv6xxx-ng).
#
# Usage:
#   ./gen-kernel-patch.sh <kernel-src-dir> [output.patch]
#
# The patch adds:
#   - drivers/net/ssv6xxx-ng/  (driver sources, in-tree Makefile)
#   - a Kconfig hook in drivers/net/wireless/Kconfig
#   - a Makefile hook in drivers/net/wireless/Makefile
#
# Apply with:
#   cd <kernel-src-dir> && patch -p1 < <output.patch>
#
# Then enable CONFIG_SSV6051 (as module) and build the kernel.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRV_NAME="ssv6xxx-ng"

KERNEL_DIR="${1:-}"
OUT="${2:-ssv6xxx-in-tree.patch}"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -n "$KERNEL_DIR" ] || { echo "Usage: $0 <kernel-src-dir> [output.patch]" >&2; exit 1; }
[ -f "$KERNEL_DIR/Makefile" ] || die "$KERNEL_DIR does not look like a kernel tree (no Makefile)"
[ -d "$KERNEL_DIR/drivers/net/wireless" ] || die "$KERNEL_DIR/drivers/net/wireless not found"
[ -f "$SCRIPT_DIR/ssv6051.h" ] || die "driver sources not found in $SCRIPT_DIR"

# Resolve OUT to an absolute path. The output patch must never be copied
# into the staging tree: a patch inside the source dir would end up
# embedded in the next patch (patch grows on every run).
OUT=$(readlink -f "$OUT")

KVER=$(sed -n 's/^VERSION = \(.*\)/\1/p' "$KERNEL_DIR/Makefile" | head -n1)
KPTC=$(sed -n 's/^PATCHLEVEL = \(.*\)/\1/p' "$KERNEL_DIR/Makefile" | head -n1)
KSUB=$(sed -n 's/^SUBLEVEL = \(.*\)/\1/p' "$KERNEL_DIR/Makefile" | head -n1)
KVER_STR="${KVER:-?}.${KPTC:-?}.${KSUB:-?}"

STAGING=$(mktemp -d)
cleanup() { rm -rf "$STAGING"; }
trap cleanup EXIT INT TERM

# Mirror only the subtree the patch touches: "a" = pristine, "b" = with
# the driver added and the Kconfig/Makefile hooks applied.
mkdir -p "$STAGING/a/drivers/net" "$STAGING/b/drivers/net"
cp -r "$KERNEL_DIR/drivers/net/wireless" "$STAGING/a/drivers/net/wireless"
cp -r "$KERNEL_DIR/drivers/net/wireless" "$STAGING/b/drivers/net/wireless"

# Copy the driver sources into "b"
cp -r "$SCRIPT_DIR" "$STAGING/b/drivers/net/wireless/$DRV_NAME"

# Prune everything that is not a source file: build artifacts, stale
# patches (prevents patch-in-patch growth), this script, VCS dirs.
(
	cd "$STAGING/b/drivers/net/wireless/$DRV_NAME"
	find . -depth \
		\( -name '*.o' -o -name '*.ko' -o -name '*.mod' \
		-o -name '*.mod.c' -o -name '*.cmd' -o -name '*.patch' \
		-o -name 'Module.symvers' -o -name 'modules.order' \
		-o -name '.tmp_versions' -o -name '.git' \
		-o -name 'gen-kernel-patch.sh' \
		-o -name "$(basename "$OUT")" \) \
		-exec rm -rf {} +
)

# Replace the out-of-tree Makefile with the in-tree variant
cat > "$STAGING/b/drivers/net/wireless/$DRV_NAME/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
#
# ssv6xxx-ng - SSV6051 SDIO WLAN driver (in-tree)

obj-$(CONFIG_SSV6051) += ssv6051.o
ssv6051-objs := main.o sdio.o fw.o efuse.o init.o hci.o key.o tx.o rx.o debugfs.o

ccflags-y += -I$(src)
# Compile dev_dbg() in: the ssv6051 debug= module parameter still gates
# what is actually printed (see DBG_* flags in ssv6051.h).
ccflags-y += -DDEBUG
EOF

# Hook the driver Kconfig into drivers/net/wireless/Kconfig
KCONFIG="$STAGING/b/drivers/net/wireless/Kconfig"
if ! grep -q "$DRV_NAME/Kconfig" "$KCONFIG"; then
	{
		echo ""
		echo "source \"drivers/net/wireless/$DRV_NAME/Kconfig\""
	} >> "$KCONFIG"
fi

# Hook the driver build into drivers/net/wireless/Makefile
WMAKE="$STAGING/b/drivers/net/wireless/Makefile"
if ! grep -q "$DRV_NAME/" "$WMAKE"; then
	{
		echo ""
		echo 'obj-$(CONFIG_SSV6051) += '"$DRV_NAME/"
	} >> "$WMAKE"
fi

# Sanity checks before emitting the patch
if [ -n "$(find "$STAGING/b/drivers/net/wireless/$DRV_NAME" -type f ! -name '*.c' ! -name '*.h' ! -name 'Makefile' ! -name 'Kconfig' ! -name 'TODO.md' 2>/dev/null)" ]; then
	die "unexpected non-source files in the driver copy"
fi

# Generate the patch (a/... b/... prefixes, patch -p1 friendly)
cd "$STAGING"
{
	echo "# ssv6xxx-ng: SSV6051 SDIO WLAN driver (rewritten, kernel >= 6.6)"
	echo "#"
	echo "# Generated against kernel $KVER_STR by $DRV_NAME/gen-kernel-patch.sh"
	echo "#"
	echo "# Adds drivers/net/wireless/$DRV_NAME and hooks it into the"
	echo "# wireless Kconfig/Makefile. Enable CONFIG_SSV6051=m and build."
	echo "#"
	echo "# The firmware image (ssv6051-wifi.bin) must be in /lib/firmware."
	# Normalize the diff headers: strip timestamps so repeated runs
	# produce byte-identical patches.
	diff -ruN a b || true
} | awk '/^(---|\\+\\+\\+) / { sub(/\t.*/, "") } { print }' > "$OUT"

echo "Patch written to: $OUT ($(wc -c < "$OUT") bytes)"
echo "Apply with: cd $KERNEL_DIR && patch -p1 < $OUT"
