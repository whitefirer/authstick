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

echo "=== Done ==="
echo "Merged: $WEB_DIR/authstick-merged.bin"
echo "Start web-flash: cd $WEB_DIR && node -e \"...\"  (port 8999)"
echo "Or: cd $WEB_DIR && python3 -m http.server 8999"
