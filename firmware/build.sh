#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
WEB_DIR="$PROJECT_DIR/web-flash"

# Source ESP-IDF
source /home/tenbox/esp/esp-idf/export.sh

cd "$SCRIPT_DIR"

# Build firmware
echo "=== Building firmware ==="
idf.py build

# Merge into single binary for web-flash
echo "=== Merging firmware ==="
esptool.py --chip esp32s3 merge_bin \
    -o "$WEB_DIR/authstick-merged.bin" \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/authstick.bin

# Also copy standalone app binary
cp build/authstick.bin "$WEB_DIR/authstick.bin"

# Compute SHA256 and write meta
SHA256=$(sha256sum "$WEB_DIR/authstick-merged.bin" | awk '{print $1}')
SIZE=$(stat -c%s "$WEB_DIR/authstick-merged.bin")

cat > "$WEB_DIR/fw-meta.json" << JSONEOF
{
  "version": "$(date +%Y%m%d-%H%M)",
  "sha256": "$SHA256",
  "size": $SIZE,
  "url": "authstick-merged.bin"
}
JSONEOF

echo ""
echo "=== Done ==="
echo "Merged:  $WEB_DIR/authstick-merged.bin ($(du -h "$WEB_DIR/authstick-merged.bin" | cut -f1))"
echo "SHA256:  $SHA256"
echo ""
echo "Flash page:     http://localhost:8999/flash.html"
echo "Dev tools:      http://localhost:8999/dev.html"
echo "Debug terminal: http://localhost:8999/debug.html"
echo ""
