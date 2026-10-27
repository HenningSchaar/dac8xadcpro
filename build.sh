#!/bin/bash
set -euo pipefail

KERNEL_VERSION=$(uname -r)
MACHINE_PATH="/lib/modules/${KERNEL_VERSION}/kernel/sound/soc/bcm"
DT_PATH="/boot/overlays"
DTS_FILE="hifiberry-dac8xadcpro-overlay.dts"
DTBO_FILE="${DTS_FILE%.dts}.dtbo"
MACHINE_MODULE="hifiberry-dac8xadcpro"

# helper to find .ko or .ko.xz in cwd
find_module_file() {
    local base="$1"
    if [ -f "${base}.ko" ]; then
        echo "${base}.ko"
        return 0
    fi
    if [ -f "${base}.ko.xz" ]; then
        echo "${base}.ko.xz"
        return 0
    fi
    # try any matching prefix
    local f
    f=$(ls -1 ${base}*.ko* 2>/dev/null | head -n1 || true)
    if [ -n "$f" ]; then
        echo "$f"
        return 0
    fi
    return 1
}

echo "Copying machine module to ${MACHINE_PATH}..."
sudo mkdir -p "${MACHINE_PATH}"
src=$(find_module_file "${MACHINE_MODULE}") || { echo "Error: built module for ${MACHINE_MODULE} not found"; exit 1; }
sudo cp -v "$src" "${MACHINE_PATH}/"

echo "Running depmod..."
sudo depmod

# Build DT overlay, capture dtc output to logfile
if [ -f "${DTS_FILE}" ]; then
  echo "Building device tree overlay from ${DTS_FILE}..."
  dtc -I dts -O dtb -o "${DTBO_FILE}" "${DTS_FILE}" >dtc-build.log 2>&1 || true
  sudo mkdir -p "${DT_PATH}"
  sudo cp -v "${DTBO_FILE}" "${DT_PATH}/"
fi

echo "Done."