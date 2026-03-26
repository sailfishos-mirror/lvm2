# Phase 3: Storage Import (Initiator Setup) - COMPLETE

## Implementation Date
2026-03-26

## Summary
Phase 3 of the LVM cluster testing framework has been successfully implemented. This phase adds the ability for test nodes (nodes 1..N) to import shared storage from node 0 (storage exporter) using iSCSI and/or NVMe-oF protocols, including multipath support.

## Files Created

### 1. `/work/shared/lvm-work.git/test/cluster/cluster-storage-importer.sh`
Main storage import script that runs on test nodes (1..N).

**Key Functions:**

- `cluster_iscsi_setup_initiator()` - Configures iSCSI initiator:
  - Ensures iscsid service is running
  - Discovers targets from node 0 using `iscsiadm`
  - Logs in to target IQN: `iqn.2025-03.com.lvm:cluster.<cluster_id>`
  - Verifies session is established
  - Waits for devices to appear

- `cluster_nvme_setup_initiator()` - Configures NVMe-oF initiator:
  - Loads nvme-tcp kernel module
  - Discovers NVMe subsystems from node 0
  - Connects to subsystem NQN: `nqn.2025-03.lvm.cluster:<cluster_id>`
  - Uses TCP transport on port 4420
  - Verifies connection is established
  - Waits for devices to appear

- `cluster_multipath_setup()` - Configures device-mapper multipath:
  - Backs up existing multipath.conf if present
  - Creates multipath configuration with:
    - User-friendly names enabled
    - Find multipaths enabled
    - Blacklist for local devices (loop, ram, vd*, etc.)
    - Whitelist for iSCSI and NVMe devices
  - Restarts multipathd service
  - Reconfigures multipath devices with `multipath -r`

- `cluster_storage_discover_devices()` - Discovers imported devices:
  - Rescans SCSI bus for iSCSI devices
  - Rescans NVMe controllers for NVMe devices
  - Waits for udev to settle
  - **For multipath mode:** Discovers `/dev/mapper/*` devices
  - **For non-multipath mode:**
    - **iSCSI:** Finds `/dev/sd*` devices from LIO-ORG vendor
    - **NVMe:** Finds `/dev/nvme*n*` devices
    - **both:** Finds both iSCSI and NVMe devices
  - Lists all discovered devices with full paths

- `cluster_storage_import_disconnect()` - Disconnects from storage:
  - Logs out of iSCSI sessions using `iscsiadm --logout`
  - Disconnects NVMe-oF connections using `nvme disconnect`
  - Removes stale node records
  - Used during cluster cleanup

- `cluster_storage_import()` - Main orchestration function:
  - Validates environment variables
  - Sets up multipath if enabled (before initiator setup)
  - Configures iSCSI and/or NVMe-oF initiators based on CLUSTER_STORAGE_TYPE
  - Discovers imported devices
  - Can be executed as standalone script or sourced as library

**Script can be run standalone:**
```bash
export CLUSTER_STORAGE_TYPE=iscsi
export CLUSTER_MULTIPATH_ENABLE=0
export NODE0_IP=192.168.122.100
export THIS_NODE_IP=192.168.122.101

./cluster-storage-importer.sh <cluster_id> <node_num>
```

## Integration with Existing Code

### Integration Points

**With Phase 1 (Core Infrastructure):**
- Uses `cluster-test-lib.sh` for logging functions
- Will be called via SSH from cluster orchestration scripts

**With Phase 2 (Storage Export):**
- Connects to storage exported from node 0
- Uses matching IQN/NQN naming scheme:
  - iSCSI: `iqn.2025-03.com.lvm:cluster.<cluster_id>`
  - NVMe: `nqn.2025-03.lvm.cluster:<cluster_id>`
- Discovers devices that were exported from node 0

**For Phase 4 (Test Execution):**
Phase 4 will need to:
- Call `cluster_storage_import()` on each test node after creation
- Use `cluster_storage_discover_devices()` to populate device variables ($dev1, $dev2, etc.)
- Ensure devices are available before running tests

## Configuration Variables Used

All configuration variables from `configs/default-cluster.conf`:

- `CLUSTER_STORAGE_TYPE` - iscsi, nvme, or both (default: iscsi)
- `CLUSTER_MULTIPATH_ENABLE` - 0 or 1 (default: 0)
- `CLUSTER_MULTIPATH_PATHS` - Number of paths per device (default: 2, for future use)

## Device Discovery Behavior

### Non-Multipath Mode (CLUSTER_MULTIPATH_ENABLE=0)

**iSCSI Storage:**
- Devices appear as `/dev/sdb`, `/dev/sdc`, `/dev/sdd`, etc.
- Identified by vendor "LIO-ORG" in lsblk output
- Device paths may differ between nodes, but logical ordering is consistent

**NVMe Storage:**
- Devices appear as `/dev/nvme0n1`, `/dev/nvme0n2`, `/dev/nvme1n1`, etc.
- Identified by subsystem NQN in nvme list output
- Device paths follow kernel assignment order

**Both Storage Types:**
- iSCSI devices listed first, then NVMe devices
- Each device maps to a variable: $dev1, $dev2, $dev3, etc.

### Multipath Mode (CLUSTER_MULTIPATH_ENABLE=1)

**Multipath Devices:**
- Devices appear as `/dev/mapper/mpath*` (e.g., /dev/mapper/mpatha, /dev/mapper/mpathb)
- Uses user-friendly names
- Each multipath device aggregates multiple paths
- All nodes see consistent device names

## Package Requirements

Test nodes (1..N) need these packages installed:
- **RHEL/Fedora:** iscsi-initiator-utils, nvme-cli, device-mapper-multipath (if multipath enabled)
- **Debian/Ubuntu:** open-iscsi, nvme-cli, multipath-tools (if multipath enabled)

These should be added to the package installation phase in cluster-vm-manager.sh.

## Testing Status

**Implementation Complete:**
1. ✅ Code implementation complete
2. ✅ iSCSI initiator configuration function
3. ✅ NVMe-oF initiator configuration function
4. ✅ Device discovery function
5. ✅ Multipath configuration function
6. ✅ Disconnect/cleanup function

**Manual Testing Required:**
1. ⏳ Test iSCSI initiator on node 1 connecting to node 0
2. ⏳ Test NVMe-oF initiator on node 1 connecting to node 0
3. ⏳ Verify devices appear correctly on test nodes
4. ⏳ Verify all test nodes see the same devices from node 0
5. ⏳ Test multipath device creation (if enabled)
6. ⏳ Integration test with full cluster creation

**Testing will be performed as part of:**
- Phase 4 integration (when test execution layer uses discovered devices)
- End-to-end cluster testing (when all phases are complete)

## Usage Example

### On Test Node (Node 1)

After cluster creation and storage export on node 0, import storage:

```bash
# SSH to test node 1
ssh root@<node1_ip>

# Import storage (automated, but can be done manually)
export CLUSTER_STORAGE_TYPE=iscsi
export CLUSTER_MULTIPATH_ENABLE=0
export NODE0_IP=192.168.122.100
export THIS_NODE_IP=192.168.122.101

/path/to/cluster-storage-importer.sh lvmtest-12345-67890 1

# Verify devices are visible
lsblk
# Should show /dev/sdb, /dev/sdc, etc. with LIO-ORG vendor

# For iSCSI, check sessions
iscsiadm -m session
# Should show active session to node 0

# For NVMe, check connections
nvme list
# Should show NVMe devices from node 0
```

### With Multipath Enabled

```bash
# After storage import with CLUSTER_MULTIPATH_ENABLE=1
multipath -ll
# Shows:
#   mpatha (36001405...) dm-0 LIO-ORG,FILEIO
#   size=1.0G features='0' hwhandler='1 alua' wp=rw
#   `-+- policy='service-time 0' prio=50 status=active
#     `- 2:0:0:0 sdb 8:16 active ready running

lsblk
# Shows /dev/mapper/mpatha, /dev/mapper/mpathb, etc.
```

## Known Limitations

1. **Multipath configuration:** Basic multipath.conf template provided; may need tuning for specific use cases
2. **Device ordering:** Device discovery order may vary slightly between nodes (but same devices are available)
3. **Network dependency:** Storage import requires network connectivity to node 0
4. **Retry logic:** No automatic retry if initial connection fails (can be added in future)

## Error Handling

The script includes comprehensive error handling:
- Checks for required commands (iscsiadm, nvme, multipath)
- Verifies services are running (iscsid, multipathd)
- Validates environment variables
- Confirms sessions/connections are established
- Returns non-zero exit codes on failures

## Next Steps (Phase 4)

Phase 4 will implement the test execution layer:
1. Create `cluster-executor.sh` for SSH-based command execution
2. Implement `cluster_node_exec()` for node-specific execution
3. Implement `cluster_all_exec()` for serial execution on all test nodes
4. Implement `cluster_nodes_exec()` for parallel execution
5. Set up test variable system ($node1, $node2, $nodes, $nodep)
6. Set up device variable system ($dev1, $dev2, $dev3, etc.)
7. Deploy test files to test nodes
8. Integrate storage import into cluster creation workflow

## Verification Commands

### SSH to Cluster Nodes

Use the `lvmtestssh` helper script to easily access cluster nodes:

```bash
# SSH to node 0 (storage exporter)
./lvmtestssh 0

# SSH to node 1 (test node)
./lvmtestssh 1

# Run a command on a node
./lvmtestssh 1 "lsblk"

# Auto-detects most recent cluster, or specify:
CLUSTER_ID=lvmtest-1234-5678 ./lvmtestssh 1
```

### After Storage Import

After storage import on test node:

```bash
# SSH to test node
./lvmtestssh 1

# For iSCSI:
iscsiadm -m session
lsblk -o NAME,SIZE,VENDOR,MODEL | grep LIO-ORG

# For NVMe:
nvme list
lsblk -o NAME,SIZE | grep nvme

# For multipath:
multipath -ll
ls -l /dev/mapper/

# Device count verification:
lsblk -d -n -o NAME,VENDOR 2>/dev/null | grep -c 'LIO-ORG'
# Should match CLUSTER_NUM_DEVICES

# Check udev has settled:
udevadm settle
```

## Integration with VM Manager

Phase 4 will modify `cluster-vm-manager.sh` to:
1. Call `cluster_storage_import()` on each test node after creation
2. Pass cluster_id and node_num as arguments
3. Set required environment variables before calling
4. Verify devices are discovered before proceeding to lock manager setup

## Related Documentation
- [Phase 1 Complete](PHASE1_COMPLETE.md) - Core infrastructure
- [Phase 2 Complete](PHASE2_COMPLETE.md) - Storage export layer
- [Plan Document](_plans/cluster-testing-framework.md) - Overall plan

## Integration Status

✅ **AUTOMATED INTEGRATION COMPLETE** (2026-03-26)

Storage import is now fully integrated into automated cluster creation:
- `cluster_vm_setup_storage_import()` function added to `cluster-vm-manager.sh`
- Called automatically during cluster creation after package installation
- Initiator packages (iscsi-initiator-utils, nvme-cli) automatically installed on test nodes
- Multipath installation conditional on `CLUSTER_MULTIPATH_ENABLE` setting

See [PHASE3_INTEGRATION_COMPLETE.md](PHASE3_INTEGRATION_COMPLETE.md) for integration details.

## Status
✅ **PHASE 3 COMPLETE** - Storage import layer implemented with iSCSI, NVMe-oF, and multipath support
✅ **INTEGRATION COMPLETE** - Fully automated in cluster creation workflow
