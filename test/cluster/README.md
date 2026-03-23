# LVM Cluster Testing Framework

This directory contains the infrastructure for testing LVM shared VGs in a multi-node cluster environment.

## Status: Phase 1 Complete

Phase 1 provides core infrastructure for VM creation, management, and basic cluster operations.

### Implemented Features

- ✅ VM lifecycle management (create, destroy)
- ✅ Automated VM provisioning with cloud-init
- ✅ SSH key setup and connectivity
- ✅ Package installation (node-specific)
- ✅ LVM source deployment and compilation
- ✅ Sanlock source deployment and compilation
- ✅ Lock manager configuration (sanlock)
- ✅ Cluster state persistence
- ✅ Configuration management

### Not Yet Implemented

- Storage export/import (iSCSI/NVMe) - Phase 2
- Test execution layer - Phase 4
- Integration with LVM test suite - Phase 6

## Quick Start

### Prerequisites

1. **Root access** - Required for libvirt/VM operations
2. **Libvirt tools** - `virsh`, `virt-install`
3. **Cloud-ready OS image** - Fedora/CentOS Stream cloud image

```bash
# Install dependencies (Fedora/RHEL)
dnf install libvirt virt-install qemu-kvm cloud-utils

# Download a cloud image
cd /var/lib/libvirt/images
curl -O https://download.fedoraproject.org/pub/fedora/linux/releases/40/Cloud/x86_64/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
```

### Basic Usage

#### 1. Configure your cluster

Edit `configs/default-cluster.conf` or create a custom config:

```bash
# Minimum required settings
CLUSTER_NUM_NODES=3
CLUSTER_NODE_OS_IMAGE=/var/lib/libvirt/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
CLUSTER_LOCK_TYPE=sanlock
CLUSTER_STORAGE_TYPE=iscsi
```

#### 2. Create a cluster

```bash
cd /work/shared/lvm-work.git/test/cluster

# Create with default config
sudo ./cluster-test-main.sh create

# Or with custom config
sudo ./cluster-test-main.sh -c /path/to/my-config.conf create
```

This will:
- Generate a unique cluster ID
- Create N+1 VMs (node 0 + test nodes 1..N)
- Configure SSH access
- Install required packages
- Set up lock manager on test nodes

Output example:
```
[INFO] Cluster ID: lvmtest-1234567890-12345
[INFO] Node IPs:
  Node 0: 192.168.122.10
  Node 1: 192.168.122.11
  Node 2: 192.168.122.12
  Node 3: 192.168.122.13
```

#### 3. Check cluster status

```bash
sudo ./cluster-test-main.sh -i lvmtest-1234567890-12345 status
```

#### 4. List all clusters

```bash
sudo ./cluster-test-main.sh list
```

#### 5. Destroy a cluster

```bash
sudo ./cluster-test-main.sh -i lvmtest-1234567890-12345 destroy
```

## Testing In-Development Code

### LVM Source Deployment

To test LVM from source instead of packages:

```bash
cat > /tmp/lvm-source-test.conf <<EOF
CLUSTER_NUM_NODES=3
CLUSTER_NODE_OS_IMAGE=/var/lib/libvirt/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
CLUSTER_LOCK_TYPE=sanlock
CLUSTER_STORAGE_TYPE=iscsi

# Enable LVM source deployment
CLUSTER_USE_SOURCE=1
CLUSTER_SOURCE_DIR=/work/shared/lvm-work.git
CLUSTER_BUILD_OPTS="--enable-debug --enable-testing"
EOF

cd /work/shared/lvm-work.git
sudo ./test/cluster/cluster-test-main.sh -c /tmp/lvm-source-test.conf create
```

This will:
- Copy LVM source to test nodes
- Install build dependencies
- Compile LVM on each node
- Install to `/usr/local`
- Configure PATH/LD_LIBRARY_PATH

### Sanlock Source Deployment

To test sanlock from source:

```bash
cat > /tmp/sanlock-source-test.conf <<EOF
CLUSTER_NUM_NODES=3
CLUSTER_NODE_OS_IMAGE=/var/lib/libvirt/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
CLUSTER_LOCK_TYPE=sanlock
CLUSTER_STORAGE_TYPE=iscsi

# Enable sanlock source deployment
SANLOCK_USE_SOURCE=1
SANLOCK_SOURCE_DIR=/work/shared/sanlock.git
SANLOCK_BUILD_OPTS=""
EOF

sudo ./test/cluster/cluster-test-main.sh -c /tmp/sanlock-source-test.conf create
```

### Combined LVM + Sanlock Source

```bash
cat > /tmp/full-source-test.conf <<EOF
CLUSTER_NUM_NODES=3
CLUSTER_NODE_OS_IMAGE=/var/lib/libvirt/images/Fedora-Cloud-Base-40-1.14.x86_64.qcow2
CLUSTER_LOCK_TYPE=sanlock
CLUSTER_STORAGE_TYPE=iscsi

# Enable both
CLUSTER_USE_SOURCE=1
CLUSTER_SOURCE_DIR=/work/shared/lvm-work.git

SANLOCK_USE_SOURCE=1
SANLOCK_SOURCE_DIR=/work/shared/sanlock.git
EOF

sudo ./test/cluster/cluster-test-main.sh -c /tmp/full-source-test.conf create
```

Note: Sanlock is deployed before LVM so that lvmlockd can link against the compiled sanlock libraries.

## Architecture

### Node Numbering

- **Node 0**: Storage exporter (SAN simulator)
  - Runs iSCSI/NVMe target services
  - Does NOT run tests or lock manager

- **Nodes 1..N**: Test nodes
  - Run LVM tests
  - Import storage from node 0
  - Run lock manager (sanlock/dlm)

Total VMs = N + 1 (where N = CLUSTER_NUM_NODES)

### Directory Structure

```
test/cluster/
├── cluster-test-main.sh          # Main entry point
├── cluster-test-lib.sh           # Core library functions
├── cluster-vm-manager.sh         # VM lifecycle management
├── configs/
│   └── default-cluster.conf      # Default configuration
└── README.md                     # This file
```

### Configuration Variables

See `configs/default-cluster.conf` for all available options.

Key settings:
- `CLUSTER_NUM_NODES` - Number of test nodes (default: 3)
- `CLUSTER_NODE_OS_IMAGE` - Path to cloud image (required)
- `CLUSTER_LOCK_TYPE` - sanlock or dlm (default: sanlock)
- `CLUSTER_STORAGE_TYPE` - iscsi, nvme, or both (default: iscsi)
- `CLUSTER_USE_SOURCE` - Deploy LVM from source (default: 0)
- `SANLOCK_USE_SOURCE` - Deploy sanlock from source (default: 0)

## Troubleshooting

### VM Creation Fails

Check libvirt is running:
```bash
systemctl status libvirtd
```

Verify OS image exists:
```bash
ls -lh /var/lib/libvirt/images/
```

### SSH Connection Issues

Check VM is running and has IP:
```bash
virsh list
virsh domifaddr lvmtest-XXX-node1
```

Test SSH manually:
```bash
ssh -i ~/.ssh/cluster_test_rsa root@<vm-ip>
```

### Enable Debug Mode

```bash
export CLUSTER_DEBUG=1
sudo ./cluster-test-main.sh create
```

### Clean Up Orphaned VMs

```bash
# List all cluster VMs
virsh list --all | grep lvmtest

# Destroy specific VM
virsh destroy lvmtest-XXX-node0
virsh undefine lvmtest-XXX-node0 --remove-all-storage
```

## Next Steps (Upcoming Phases)

- **Phase 2**: Storage export (iSCSI/NVMe target setup on node 0)
- **Phase 3**: Storage import (initiator setup on test nodes)
- **Phase 4**: Test execution layer ($node1, $nodes, $nodep variables)
- **Phase 5**: Main orchestration improvements
- **Phase 6**: Integration with LVM test suite

## Development

### Adding New Features

1. Add function to appropriate module:
   - `cluster-test-lib.sh` - Core utilities
   - `cluster-vm-manager.sh` - VM operations

2. Export function if needed for other modules

3. Update configuration in `configs/default-cluster.conf`

4. Update this README

### Testing Changes

```bash
# Create test cluster
sudo ./cluster-test-main.sh -c configs/default-cluster.conf create

# Verify functionality
sudo ./cluster-test-main.sh -i <cluster-id> status

# Clean up
sudo ./cluster-test-main.sh -i <cluster-id> destroy
```

## License

Same as LVM2 project.
