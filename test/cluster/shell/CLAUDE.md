# DEVICE TYPE TESTING

Tests exercise three device types: SCSI, NVMe, and multipath. There are
two approaches to covering multiple device types, and each test should use
the approach that fits its needs.

## Approach 1: Per-type loop in the test script

The test uses type-specific variables ($scsi1, $nvme1, $mpath1) and
CLUSTER_NUM_SCSI/NVME/MULTIPATH to iterate over available device types
within a single run on a mixed-device config. Each iteration runs the
full test body with devices of one type.

Use when test logic changes by device type: PR type selection (WE vs WEAR),
parallel-race eligibility, device rediscovery method, etc. Examples:
local-vg-pr.sh, shared-vg-pr.sh, local-vg-lvmpersist.sh.

Pattern:
  DEV_TYPES=""
  [ "${CLUSTER_NUM_SCSI:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES scsi"
  [ "${CLUSTER_NUM_NVME:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES nvme"
  [ "${CLUSTER_NUM_MULTIPATH:-0}" -ge 2 ] && DEV_TYPES="$DEV_TYPES mpath"
  for devtype in $DEV_TYPES; do
      case $devtype in
          scsi)  d1=$scsi1; d2=$scsi2 ;;
          nvme)  d1=$nvme1; d2=$nvme2 ;;
          mpath) d1=$mpath1; d2=$mpath2 ;;
      esac
      # ... test body using $d1, $d2, with $devtype for conditional logic
  done

## Approach 2: Generic $dev variables with single-type configs

The test uses only generic $dev1, $dev2, etc. and is device-type-agnostic.
Multi-type coverage comes from running the same test group against different
configs (e.g. 4-scsi, 4-nvme, 4-mpath), each creating a separate cluster.

Use when test logic is identical regardless of device type: LV creation,
activation, locking, snapshots, thin, cache, raid, etc. This keeps scripts
simple and lets the config matrix also vary sanlock parameters (CAW,
io_timeout, sector_size). Examples: most shared-vg-*.sh tests.
