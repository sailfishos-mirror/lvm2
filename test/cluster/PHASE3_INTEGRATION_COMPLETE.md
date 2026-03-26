# Phase 3: Storage Import Integration - COMPLETE

## Integration Date
2026-03-26

## Summary
Storage import functionality has been fully integrated into the automated cluster creation workflow. Test nodes now automatically import storage from node 0 during cluster creation.

## Changes Made

### 1. Added `cluster_vm_setup_storage_import()` Function

**Location:** `cluster-vm-manager.sh` (after `cluster_vm_setup_storage_export()`)

**Purpose:** Copies storage importer script to test nodes and executes it to import storage from node 0.

**Implementation:**
```bash
cluster_vm_setup_storage_import() {
    local node_ip="$1"
    local node_num="$2"
    local node0_ip="$3"
    local cluster_id="$4"

    # Copies cluster-storage-importer.sh and cluster-test-lib.sh to test node
    # Executes storage import with environment variables:
    #   - CLUSTER_STORAGE_TYPE
    #   - CLUSTER_MULTIPATH_ENABLE
    #   - NODE0_IP
    #   - THIS_NODE_IP
}
```

### 2. Integrated into Cluster Creation Workflow

**Location:** `cluster-vm-manager.sh` in `cluster_vms_create_all()`

**Workflow Order:**
1. Create node 0 (storage exporter)
2. Install packages on node 0
3. **Setup storage export on node 0** ← Phase 2
4. Create test nodes (1..N)
5. Install packages on test nodes
6. Deploy source code (if enabled)
7. **Import storage on test nodes** ← **NEW (Phase 3)**
8. Setup lock managers on test nodes

**Code:**
```bash
# Import storage on test nodes from node 0
cluster_log "Setting up storage import on test nodes"
for node_num in $(seq 1 "$num_nodes"); do
    local node_ip="${CLUSTER_NODE_IPS[$node_num]}"
    cluster_vm_setup_storage_import "$node_ip" "$node_num" "$node0_ip" "$cluster_id" || {
        cluster_error "Failed to setup storage import on node $node_num"
        return 1
    }
done
```

### 3. Optimized Package Installation

**Location:** `cluster-vm-manager.sh` in `cluster_vm_install_packages()`

**Changes:**
- ✅ Initiator packages already installed: `iscsi-initiator-utils`, `nvme-cli`, `sg3_utils`
- ✅ Made multipath installation conditional on `CLUSTER_MULTIPATH_ENABLE`

**Before:**
```bash
packages+=(iscsi-initiator-utils nvme-cli sg3_utils device-mapper-multipath)
```

**After:**
```bash
packages+=(iscsi-initiator-utils nvme-cli sg3_utils)
if [ "${CLUSTER_MULTIPATH_ENABLE:-0}" = "1" ]; then
    packages+=(device-mapper-multipath)
fi
```

### 4. Exported New Function

**Location:** `cluster-vm-manager.sh` (end of file)

**Added:**
```bash
export -f cluster_vm_setup_storage_import
```

### 5. Improved Cluster State Saving

**Location:** `cluster-test-lib.sh` in `cluster_state_save()`

**Changes:** Explicitly saves important configuration variables to state file:
- `CLUSTER_STORAGE_TYPE`
- `CLUSTER_NUM_DEVICES`
- `CLUSTER_MULTIPATH_ENABLE`
- `CLUSTER_BACKING_TYPE`
- `CLUSTER_SSH_KEY_DIR`
- And more...

This ensures test scripts can load configuration even after cluster creation.

### 6. Enhanced Test Script

**Location:** `test-storage-import-manual.sh`

**Changes:**
- ✅ Accepts optional config file parameter
- ✅ Uses sensible defaults if config not provided
- ✅ Handles SSH key paths correctly when running with sudo
- ✅ Shows configuration being used

## Testing Status

### Manual Testing ✅
- ✅ Tested with existing cluster using `test-storage-import-manual.sh`
- ✅ Storage import successful on test node 1
- ✅ Devices discovered correctly
- ✅ iSCSI sessions established

### Automated Testing ⏳
Next step: Test full automated workflow

## How to Test Automated Integration

### Create a New Cluster (Storage Import Automatic)

```bash
cd test/cluster

# Create test config
cat > /tmp/test-auto-import.conf <<'EOF'
CLUSTER_NUM_NODES=2
CLUSTER_STORAGE_TYPE=iscsi
CLUSTER_BACKING_TYPE=sparsefile
CLUSTER_NUM_DEVICES=3
CLUSTER_DEVICE_SIZE=512
CLUSTER_NODE_OS_IMAGE=/var/lib/libvirt/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
CLUSTER_NODE_OS_VARIANT=fedora40
CLUSTER_MULTIPATH_ENABLE=0
EOF

# Create cluster (storage import happens automatically!)
sudo ./cluster-test-main.sh -c /tmp/test-auto-import.conf create
```

### Verify Storage Import Happened

```bash
# Get cluster ID from create output, then check node 1
./lvmtestssh 1 "iscsiadm -m session"
./lvmtestssh 1 "lsblk -o NAME,SIZE,VENDOR,MODEL | grep LIO-ORG"

# Check node 2
./lvmtestssh 2 "iscsiadm -m session"
./lvmtestssh 2 "lsblk -o NAME,SIZE,VENDOR,MODEL | grep LIO-ORG"

# Both nodes should show:
# - Active iSCSI session to node 0
# - 3 devices from LIO-ORG vendor
```

### Verify Device Count

```bash
# Should match CLUSTER_NUM_DEVICES (3 in this example)
./lvmtestssh 1 "lsblk -d -n -o NAME,VENDOR | grep -c 'LIO-ORG'"
./lvmtestssh 2 "lsblk -d -n -o NAME,VENDOR | grep -c 'LIO-ORG'"
```

### Test Device Access

```bash
# Test reading from imported devices
./lvmtestssh 1 "dd if=/dev/sdb of=/dev/null bs=1M count=10 iflag=direct"
./lvmtestssh 2 "dd if=/dev/sdb of=/dev/null bs=1M count=10 iflag=direct"
```

## Expected Cluster Creation Output

When creating a cluster, you should now see:

```
[INFO] Creating node 0 (storage exporter)
[INFO] Installing packages on node 0 (192.168.124.x)
[INFO] Installing storage exporter packages on node 0
[INFO] Setting up storage export on node 0 (192.168.124.x)
[INFO] Executing storage export setup on node 0
[INFO] ==========================================
[INFO] Storage Export Setup on Node 0
[INFO] ==========================================
[INFO] Creating 3 backing devices (type=sparsefile, size=512MB, sector_size=512)
[INFO] Configuring iSCSI target (num_devices=3, backing_type=sparsefile)
[INFO] Creating iSCSI target: iqn.2025-03.com.lvm:cluster.lvmtest-xxx-yyy
[INFO] Storage export setup complete on node 0

[INFO] Creating node 1 (test node)
[INFO] Installing packages on node 1 (192.168.124.x)
[INFO] Installing: iscsi-initiator-utils nvme-cli sg3_utils lvm2 lvm2-lockd sanlock...

[INFO] Creating node 2 (test node)
...

[INFO] All VMs created, node IPs: 192.168.124.x 192.168.124.y 192.168.124.z

[INFO] Setting up storage import on test nodes          ← NEW!
[INFO] Setting up storage import on node 1 (192.168.124.y)
[INFO] Executing storage import on node 1
[INFO] ==========================================
[INFO] Storage Import Setup on Node 1             ← NEW!
[INFO] ==========================================
[INFO] Configuring iSCSI initiator to connect to 192.168.124.x
[INFO] Discovering iSCSI targets from 192.168.124.x
[INFO] Logging in to iSCSI target: iqn.2025-03.com.lvm:cluster.lvmtest-xxx-yyy
[INFO] iSCSI initiator configuration complete
[INFO] Discovered 3 storage device(s):
[INFO]   - /dev/sdb
[INFO]   - /dev/sdc
[INFO]   - /dev/sdd
[INFO] Storage import complete on node 1          ← NEW!

[INFO] Setting up storage import on node 2 (192.168.124.z)
...

[INFO] Setting up lock managers on test nodes
[INFO] Starting sanlock on node 1
...

[INFO] All VMs created successfully
[INFO] Node IPs: 192.168.124.x 192.168.124.y 192.168.124.z
```

## Integration Benefits

1. ✅ **Automated Workflow**: Storage import happens automatically during cluster creation
2. ✅ **No Manual Steps**: Users don't need to run manual scripts anymore
3. ✅ **Consistent Setup**: All test nodes get identical storage configuration
4. ✅ **Error Handling**: Integration fails early if storage import fails
5. ✅ **Ready for Testing**: Clusters are immediately ready for shared VG testing

## What's Next: Phase 4

With Phase 3 integrated, clusters now have:
- ✅ Node 0 exporting storage (Phase 2)
- ✅ Test nodes importing storage (Phase 3)
- ✅ Lock managers configured
- ⏳ Test execution framework (Phase 4)

Phase 4 will add:
- `cluster-executor.sh` for SSH-based command execution
- Test variables: `$node1`, `$node2`, `$nodes`, `$nodep`
- Device variables: `$dev1`, `$dev2`, `$dev3`, etc.
- Example cluster test script

## Files Modified

1. ✅ `cluster-vm-manager.sh` - Added storage import function and integration
2. ✅ `cluster-test-lib.sh` - Improved state saving
3. ✅ `test-storage-import-manual.sh` - Enhanced for better usability

## Files Created (Earlier)

1. ✅ `cluster-storage-importer.sh` - Storage import implementation
2. ✅ `PHASE3_COMPLETE.md` - Phase 3 documentation
3. ✅ `TEST_STORAGE_IMPORT.md` - Comprehensive test guide
4. ✅ `TEST_STORAGE_IMPORT_QUICK.md` - Quick test guide

## Status

✅ **PHASE 3 INTEGRATION COMPLETE** - Storage import fully automated in cluster creation workflow!

Next: Test the automated workflow, then proceed to Phase 4 (Test Execution Layer).
