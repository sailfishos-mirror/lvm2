#!/bin/bash
#
# cluster-cleanup-orphaned-storage.sh - Find and clean up orphaned VM storage
#
# Finds storage files (disk images, cloud-init ISOs) for VMs that no longer exist
#

set -e

DISK_DIR="/var/lib/libvirt/images"

echo "Checking for orphaned cluster test storage..."
echo ""

# Get list of currently defined VMs
DEFINED_VMS=$(virsh list --all 2>/dev/null | grep lvmtest | awk '{print $2}' || true)

# Debug: show defined VMs
if [ -n "$DEFINED_VMS" ]; then
    echo "Currently defined VMs:"
    echo "$DEFINED_VMS" | while read -r vm; do echo "  $vm"; done
    echo ""
else
    echo "No VMs currently defined (all files will be considered orphaned)"
    echo ""
fi

# Find all lvmtest-related files
ORPHANED_FILES=()

if [ -d "$DISK_DIR" ]; then
    echo "Scanning $DISK_DIR for cluster test files..."

    # Find all lvmtest files using ls
    FILE_COUNT=0
    while IFS= read -r file; do
        [ -z "$file" ] && continue
        [ ! -f "$DISK_DIR/$file" ] && continue

        FILE_COUNT=$((FILE_COUNT + 1))
        full_path="$DISK_DIR/$file"

        # Extract VM name from filename
        # Files are named: lvmtest-TIMESTAMP-RANDOM-nodeN.qcow2
        #              or: lvmtest-TIMESTAMP-RANDOM-nodeN-cloudinit.iso
        vm_name=$(echo "$file" | sed -e 's/-cloudinit\.iso$//' -e 's/\.qcow2$//')

        # Check if this VM is currently defined
        if [ -n "$DEFINED_VMS" ] && echo "$DEFINED_VMS" | grep -q "^${vm_name}$"; then
            # VM is defined, skip
            echo "  Skipping (VM defined): $vm_name"
            continue
        fi

        # This is an orphaned file
        ORPHANED_FILES+=("$full_path")
    done < <(ls "$DISK_DIR" 2>/dev/null | grep "^lvmtest-.*\.\(qcow2\|iso\)$")

    echo "Found $FILE_COUNT total files matching pattern"
    echo ""
fi

if [ ${#ORPHANED_FILES[@]} -eq 0 ]; then
    echo "No orphaned storage files found."
    exit 0
fi

echo "Found ${#ORPHANED_FILES[@]} orphaned storage file(s):"
echo ""

TOTAL_SIZE=0
for file in "${ORPHANED_FILES[@]}"; do
    SIZE=$(du -h "$file" | cut -f1)
    SIZE_BYTES=$(du -b "$file" | cut -f1)
    TOTAL_SIZE=$((TOTAL_SIZE + SIZE_BYTES))
    printf "  %-80s %10s\n" "$(basename "$file")" "$SIZE"
done

echo ""
TOTAL_SIZE_HUMAN=$(numfmt --to=iec-i --suffix=B $TOTAL_SIZE 2>/dev/null || echo "${TOTAL_SIZE} bytes")
echo "Total size: $TOTAL_SIZE_HUMAN"
echo ""

# Ask for confirmation
read -p "Remove these files? [y/N] " -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted. No files were removed."
    exit 0
fi

# Remove files
echo "Removing orphaned storage files..."
for file in "${ORPHANED_FILES[@]}"; do
    echo "  Removing: $(basename "$file")"
    rm -f "$file"
done

echo ""
echo "Cleanup complete. Removed ${#ORPHANED_FILES[@]} file(s)."
