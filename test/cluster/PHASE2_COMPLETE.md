# Phase 2: Storage Export (SAN Simulation) - COMPLETE

## Implementation Date
2026-03-23

## Summary
Phase 2 of the LVM cluster testing framework has been successfully implemented. This phase adds the ability to export shared storage from node 0 (storage exporter) to test nodes using iSCSI and/or NVMe-oF protocols.

## Files Created

### 1. `/work/shared/lvm-work.git/test/cluster/cluster-storage-exporter.sh`
Main storage export script that runs on node 0.

**Key Functions:**
- `cluster_storage_create_backing()` - Creates backing storage with three types:
  - **file**: Preallocated files using `dd` + targetcli fileio backstore
  - **sparsefile**: Targetcli fileio backstore with `sparse=true` flag
  - **ramdisk**: Targetcli ramdisk backstore (rd_mcp)

- `cluster_iscsi_setup_target()` - Configures iSCSI target using targetcli:
  - Creates backstores (fileio or ramdisk)
  - Sets up target IQN: `iqn.2025-03.com.lvm:cluster.<cluster_id>`
  - Creates LUNs mapped to backstores
  - Configures portals on node 0's IP address
  - Enables demo mode for ACLs (allows any initiator)
  - Persists configuration with `targetcli saveconfig`

- `cluster_nvme_setup_target()` - Configures NVMe-oF target using configfs:
  - Loads nvmet and nvme-tcp kernel modules
  - Creates subsystem with NQN: `nqn.2025-03.lvm.cluster:<cluster_id>`
  - Creates namespaces for each backing device
  - Binds to TCP port 4420 on node 0's IP
  - For ramdisk backing, uses tmpfs files (/dev/shm) since NVMe-oF requires file paths

- `cluster_storage_cleanup()` - Cleans up storage configuration:
  - Removes iSCSI targets and backstores
  - Removes NVMe-oF subsystems and namespaces
  - Deletes backing files

- `cluster_storage_setup()` - Main orchestration function:
  - Validates environment variables
  - Creates backing storage
  - Configures iSCSI and/or NVMe-oF targets based on CLUSTER_STORAGE_TYPE
  - Can be executed as standalone script or sourced as library

**Script can be run standalone:**
```bash
export CLUSTER_STORAGE_TYPE=iscsi
export CLUSTER_NUM_DEVICES=5
export CLUSTER_DEVICE_SIZE=1024
export CLUSTER_BACKING_TYPE=file
export NODE0_IP=192.168.122.100

./cluster-storage-exporter.sh <cluster_id>
```

## Files Modified

### 1. `/work/shared/lvm-work.git/test/cluster/cluster-vm-manager.sh`

**Added Function:**
- `cluster_vm_setup_storage_export()` - Sets up storage export on node 0:
  - Copies cluster scripts (cluster-test-lib.sh, cluster-storage-exporter.sh) to node 0
  - Sets environment variables for storage configuration
  - Executes storage export setup remotely via SSH
  - Called automatically during cluster creation after node 0 packages are installed

**Modified Function:**
- `cluster_vms_create_all()` - Now calls `cluster_vm_setup_storage_export()` after creating node 0:
  1. Creates node 0 (storage exporter)
  2. Installs packages on node 0
  3. **Sets up storage export on node 0** ← NEW
  4. Creates test nodes (1..N)
  5. Sets up lock managers on test nodes

- `cluster_vms_destroy_all()` - Now cleans up storage before destroying VMs:
  1. Calls `cluster_storage_cleanup()` on node 0 before destroying VMs
  2. Destroys all VMs (0..N)

**Exports:**
- Added `cluster_vm_setup_storage_export` to function exports

## Configuration Variables Used

All configuration variables from `configs/default-cluster.conf`:

- `CLUSTER_STORAGE_TYPE` - iscsi, nvme, or both (default: iscsi)
- `CLUSTER_NUM_DEVICES` - Number of devices to export (default: 5)
- `CLUSTER_DEVICE_SIZE` - Device size in MB (default: 1024)
- `CLUSTER_DEVICE_SECTOR_SIZE` - Sector size: 512 or 4096 (default: 512)
- `CLUSTER_BACKING_TYPE` - file, sparsefile, or ramdisk (default: file)

## Integration Points

### With Phase 1 (Core Infrastructure)
- Uses `cluster-test-lib.sh` for logging functions
- Uses `cluster_vm_ssh()` for remote command execution
- Uses `cluster_vm_scp()` for file transfer
- Integrated into VM creation workflow

### For Phase 3 (Storage Import)
Phase 2 exports storage from node 0. Phase 3 will implement:
- `cluster-storage-importer.sh` to import storage on test nodes (1..N)
- iSCSI initiator configuration using `iscsiadm`
- NVMe-oF initiator configuration using `nvme connect`
- Device discovery and mapping to variables ($dev1, $dev2, etc.)

## Testing Status

**Manual Testing Required:**
Phase 2 implementation is complete but requires manual testing:

1. ✅ Code implementation complete
2. ⏳ Test file backing type with iSCSI
3. ⏳ Test sparsefile backing type with iSCSI
4. ⏳ Test ramdisk backing type with iSCSI
5. ⏳ Test NVMe-oF target creation
6. ⏳ Verify targets are accessible (lsblk, targetcli ls)
7. ⏳ Integration test with full cluster creation

**Testing will be performed as part of:**
- Phase 3 integration (when initiators connect and verify devices)
- End-to-end cluster testing (when all phases are complete)

## Node 0 Package Installation

Node 0 automatically gets storage exporter packages installed:
- **RHEL/Fedora:** targetcli, python3-rtslib, nvme-cli, sg3_utils
- **Debian/Ubuntu:** targetcli-fb, nvme-cli, sg3-utils

## Storage Export Examples

### iSCSI with File Backing
```bash
# On node 0 after setup:
targetcli ls
# Shows:
#   /backstores/fileio/lvm-disk-<cluster_id>-1 -> /var/tmp/lvm-cluster-storage/disk-<cluster_id>-1.img
#   /iscsi/iqn.2025-03.com.lvm:cluster.<cluster_id>/tpg1/luns/lun0 -> backstores/fileio/lvm-disk-<cluster_id>-1
```

### NVMe-oF with Ramdisk Backing
```bash
# On node 0 after setup:
ls /sys/kernel/config/nvmet/subsystems/nqn.2025-03.lvm.cluster:<cluster_id>/namespaces/
# Shows namespaces 1, 2, 3, ... (one per device)
```

## Known Limitations

1. **NVMe-oF ramdisk backing:** Uses tmpfs files (/dev/shm) instead of pure ramdisk since NVMe-oF requires file paths
2. **ACL security:** Currently uses demo mode (open ACLs) for easy testing. Production use should configure proper initiator ACLs.
3. **Multipath:** Not yet implemented (Phase 3 will add multipath support)

## Next Steps (Phase 3)

Phase 3 will implement storage import on test nodes (1..N):
1. Create `cluster-storage-importer.sh`
2. Implement iSCSI initiator configuration (iscsiadm)
3. Implement NVMe-oF initiator configuration (nvme connect)
4. Implement device discovery and mapping to $dev1, $dev2, etc.
5. Integrate with cluster creation workflow
6. Add multipath support (if CLUSTER_MULTIPATH_ENABLE=1)

## Verification Commands

After cluster creation with Phase 2 complete, verify on node 0:

```bash
# SSH to node 0
ssh root@<node0_ip>

# For iSCSI:
targetcli ls
systemctl status target

# For NVMe-oF:
ls /sys/kernel/config/nvmet/subsystems/
cat /sys/kernel/config/nvmet/ports/1/addr_*

# Check backing storage:
ls -lh /var/tmp/lvm-cluster-storage/
```

## Related Documentation
- [Phase 1 Complete](PHASE1_COMPLETE.md) - Core infrastructure
- [Plan Document](_plans/cluster-testing-framework.md) - Overall plan

## Status
✅ **PHASE 2 COMPLETE** - Storage export layer implemented and integrated
