#!/bin/bash
#
# cluster-storage-exporter.sh - Storage export layer for LVM cluster testing
#
# This script runs on node 0 (storage exporter node) to:
# - Create backing storage (file, sparsefile, or ramdisk)
# - Configure iSCSI targets using targetcli
# - Configure NVMe-oF targets using configfs
#
# Node 0 acts as a SAN simulator, exporting shared storage to test nodes (1..N)
#

# Source the core library for logging functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-test-lib.sh" || exit 1

# Storage directory on node 0
CLUSTER_STORAGE_DIR="${CLUSTER_STORAGE_DIR:-/var/tmp/lvm-cluster-storage}"

#
# cluster_storage_create_backing - Create backing storage on node 0
#
# Creates backing devices based on CLUSTER_BACKING_TYPE:
#   - file: Use dd to preallocate files, then create targetcli fileio backstore
#   - sparsefile: Create targetcli fileio backstore with sparse=true (no dd)
#   - ramdisk: Create targetcli ramdisk backstore (rd_mcp)
#
# Arguments:
#   $1 - backing_type (file, sparsefile, or ramdisk)
#   $2 - num_devices (number of backing devices to create)
#   $3 - device_size (size in MB)
#   $4 - sector_size (512 or 4096)
#   $5 - cluster_id (for unique naming)
#
# Returns:
#   0 on success, 1 on failure
#
# Sets global arrays:
#   BACKING_DEVICES - array of backing device paths/names
#
cluster_storage_create_backing() {
    local backing_type="$1"
    local num_devices="$2"
    local device_size="$3"
    local sector_size="$4"
    local cluster_id="$5"

    cluster_log "Creating $num_devices backing devices (type=$backing_type, size=${device_size}MB, sector_size=$sector_size)"

    # Initialize backing devices array
    BACKING_DEVICES=()

    case "$backing_type" in
        file)
            # Create directory for backing files
            mkdir -p "$CLUSTER_STORAGE_DIR" || {
                cluster_error "Failed to create storage directory: $CLUSTER_STORAGE_DIR"
                return 1
            }

            # Create preallocated files with dd
            for i in $(seq 1 "$num_devices"); do
                local file_path="$CLUSTER_STORAGE_DIR/disk-${cluster_id}-${i}.img"

                cluster_debug "Creating preallocated file: $file_path"
                dd if=/dev/zero of="$file_path" bs=1M count="$device_size" status=progress 2>&1 | grep -v records || {
                    cluster_error "Failed to create backing file: $file_path"
                    return 1
                }

                # Set appropriate permissions
                chmod 600 "$file_path"

                BACKING_DEVICES+=("$file_path")
                cluster_log "Created backing file $i/$num_devices: $file_path"
            done
            ;;

        sparsefile)
            # Create directory for sparse files
            mkdir -p "$CLUSTER_STORAGE_DIR" || {
                cluster_error "Failed to create storage directory: $CLUSTER_STORAGE_DIR"
                return 1
            }

            # Create sparse files (targetcli will handle sparse=true)
            # We just need to record the paths - targetcli creates them
            for i in $(seq 1 "$num_devices"); do
                local file_path="$CLUSTER_STORAGE_DIR/disk-${cluster_id}-${i}.img"
                BACKING_DEVICES+=("$file_path")
                cluster_log "Prepared sparse file path $i/$num_devices: $file_path"
            done
            ;;

        ramdisk)
            # For ramdisk, we don't create files - targetcli rd_mcp handles it
            # We just need to track names for targetcli configuration
            for i in $(seq 1 "$num_devices"); do
                local ramdisk_name="ramdisk-${cluster_id}-${i}"
                BACKING_DEVICES+=("$ramdisk_name")
                cluster_log "Prepared ramdisk name $i/$num_devices: $ramdisk_name"
            done
            ;;

        *)
            cluster_error "Unknown backing type: $backing_type"
            return 1
            ;;
    esac

    cluster_log "Backing storage creation complete (type=$backing_type)"
    return 0
}

#
# cluster_iscsi_setup_target - Configure iSCSI target on node 0
#
# Uses targetcli to configure:
#   - Backstores (fileio or ramdisk based on backing type)
#   - Target IQN
#   - LUNs
#   - ACLs (demo mode for easy testing)
#
# Arguments:
#   $1 - exporter_ip (IP address of node 0)
#   $2 - num_devices (number of devices to export)
#   $3 - backing_type (file, sparsefile, or ramdisk)
#   $4 - device_size (size in MB)
#   $5 - sector_size (512 or 4096)
#   $6 - cluster_id (for unique naming)
#
# Uses global array:
#   BACKING_DEVICES - array of backing device paths/names from cluster_storage_create_backing
#
# Returns:
#   0 on success, 1 on failure
#
cluster_iscsi_setup_target() {
    local exporter_ip="$1"
    local num_devices="$2"
    local backing_type="$3"
    local device_size="$4"
    local sector_size="$5"
    local cluster_id="$6"

    cluster_log "Configuring iSCSI target (num_devices=$num_devices, backing_type=$backing_type)"

    # Check if targetcli is available
    if ! command -v targetcli &>/dev/null; then
        cluster_error "targetcli command not found - is targetcli package installed?"
        return 1
    fi

    # Enable persistent reservations in targetcli
    cluster_log "Enabling persistent reservations support"
    mkdir -p /etc/target/pr || {
        cluster_warn "Failed to create /etc/target/pr directory"
    }

    # Target IQN
    local target_iqn="iqn.2025-03.com.lvm:cluster.${cluster_id}"

    cluster_debug "Target IQN: $target_iqn"

    # Clear any existing configuration for this cluster
    targetcli /iscsi delete "$target_iqn" 2>/dev/null || true

    # Create target
    cluster_log "Creating iSCSI target: $target_iqn"
    targetcli /iscsi create "$target_iqn" || {
        cluster_error "Failed to create iSCSI target"
        return 1
    }

    # Create backstores and LUNs
    for i in $(seq 1 "$num_devices"); do
        local backstore_name="lvmdisk-${i}"
        local backing_device="${BACKING_DEVICES[$((i-1))]}"

        cluster_log "Creating backstore $i/$num_devices: $backstore_name"

        case "$backing_type" in
            file)
                # Create fileio backstore with preallocated file
                targetcli /backstores/fileio create "$backstore_name" "$backing_device" ${device_size}M write_back=false || {
                    cluster_error "Failed to create fileio backstore: $backstore_name"
                    return 1
                }
                ;;

            sparsefile)
                # Create fileio backstore with sparse file
                # Note: targetcli creates the file if it doesn't exist when sparse=true
                targetcli /backstores/fileio create "$backstore_name" "$backing_device" ${device_size}M write_back=false sparse=true || {
                    cluster_error "Failed to create sparse fileio backstore: $backstore_name"
                    return 1
                }
                ;;

            ramdisk)
                # Create ramdisk backstore
                targetcli /backstores/ramdisk create "$backstore_name" ${device_size}M || {
                    cluster_error "Failed to create ramdisk backstore: $backstore_name"
                    return 1
                }
                ;;

            *)
                cluster_error "Unknown backing type: $backing_type"
                return 1
                ;;
        esac

        # Create LUN mapped to backstore
        local lun_num=$((i - 1))
        cluster_log "Creating LUN $lun_num for backstore: $backstore_name"
        targetcli /iscsi/"$target_iqn"/tpg1/luns create "/backstores/fileio/$backstore_name" "$lun_num" 2>/dev/null || \
        targetcli /iscsi/"$target_iqn"/tpg1/luns create "/backstores/ramdisk/$backstore_name" "$lun_num" || {
            cluster_error "Failed to create LUN for backstore: $backstore_name"
            return 1
        }
    done

    # Use default portal (0.0.0.0:3260 is created automatically and works fine)
    # It listens on all interfaces including the node's IP
    cluster_log "Using default iSCSI portal (0.0.0.0:3260)"
    cluster_debug "Portal will be accessible via $exporter_ip:3260"

    # Enable demo mode for ACLs (allows any initiator to connect)
    cluster_log "Enabling demo mode (open ACLs)"
    targetcli /iscsi/"$target_iqn"/tpg1 set attribute authentication=0 demo_mode_write_protect=0 generate_node_acls=1 cache_dynamic_acls=1 || {
        cluster_error "Failed to configure ACL settings"
        return 1
    }

    # Save configuration
    cluster_log "Saving targetcli configuration"
    targetcli saveconfig || {
        cluster_warn "Failed to save targetcli configuration"
    }

    cluster_log "iSCSI target configuration complete"
    cluster_log "  Target IQN: $target_iqn"
    cluster_log "  Portal: 0.0.0.0:3260 (accessible via $exporter_ip:3260)"
    cluster_log "  LUNs: $num_devices"

    return 0
}

#
# cluster_nvme_setup_target - Configure NVMe-oF target on node 0
#
# Uses configfs (/sys/kernel/config/nvmet/) to configure:
#   - Subsystem with NQN
#   - Namespaces for each backing device
#   - TCP port binding
#
# Arguments:
#   $1 - exporter_ip (IP address of node 0)
#   $2 - num_devices (number of devices to export)
#   $3 - backing_type (file, sparsefile, or ramdisk)
#   $4 - device_size (size in MB)
#   $5 - sector_size (512 or 4096)
#   $6 - cluster_id (for unique naming)
#
# Uses global array:
#   BACKING_DEVICES - array of backing device paths/names from cluster_storage_create_backing
#
# Returns:
#   0 on success, 1 on failure
#
cluster_nvme_setup_target() {
    local exporter_ip="$1"
    local num_devices="$2"
    local backing_type="$3"
    local device_size="$4"
    local sector_size="$5"
    local cluster_id="$6"

    cluster_log "Configuring NVMe-oF target (num_devices=$num_devices, backing_type=$backing_type)"

    # Check if nvmet module is loaded
    if ! lsmod | grep -q nvmet; then
        cluster_log "Loading nvmet kernel module"
        modprobe nvmet || {
            cluster_error "Failed to load nvmet module"
            return 1
        }
    fi

    if ! lsmod | grep -q nvme_tcp; then
        cluster_log "Loading nvme-tcp kernel module"
        modprobe nvme-tcp || {
            cluster_error "Failed to load nvme-tcp module"
            return 1
        }
    fi

    # NVMe subsystem NQN
    local subsystem_nqn="nqn.2025-03.lvm.cluster:${cluster_id}"
    local nvmet_base="/sys/kernel/config/nvmet"
    local subsystem_path="$nvmet_base/subsystems/$subsystem_nqn"
    local port_path="$nvmet_base/ports/1"

    cluster_debug "Subsystem NQN: $subsystem_nqn"

    # Remove existing configuration if present
    if [ -d "$subsystem_path" ]; then
        cluster_debug "Removing existing NVMe subsystem"
        # Remove port link first
        if [ -L "$port_path/subsystems/$subsystem_nqn" ]; then
            rm -f "$port_path/subsystems/$subsystem_nqn" || true
        fi
        # Remove namespaces
        for ns in "$subsystem_path"/namespaces/*; do
            [ -d "$ns" ] && rmdir "$ns" 2>/dev/null || true
        done
        # Remove subsystem
        rmdir "$subsystem_path" 2>/dev/null || true
    fi

    # Create subsystem
    cluster_log "Creating NVMe subsystem: $subsystem_nqn"
    mkdir -p "$subsystem_path" || {
        cluster_error "Failed to create NVMe subsystem directory"
        return 1
    }

    # Allow any host to connect
    echo 1 > "$subsystem_path/attr_allow_any_host" || {
        cluster_error "Failed to set allow_any_host"
        return 1
    }

    # Create namespaces for each backing device
    for i in $(seq 1 "$num_devices"); do
        local ns_id="$i"
        local ns_path="$subsystem_path/namespaces/$ns_id"
        local backing_device="${BACKING_DEVICES[$((i-1))]}"

        cluster_log "Creating NVMe namespace $i/$num_devices (nsid=$ns_id)"

        # Create namespace directory
        mkdir -p "$ns_path" || {
            cluster_error "Failed to create namespace directory: $ns_path"
            return 1
        }

        # For ramdisk backing, we need to use a file-based approach
        # since NVMe-oF requires a block device or file path
        if [ "$backing_type" = "ramdisk" ]; then
            cluster_warn "NVMe-oF does not support pure ramdisk backing - using tmpfs file instead"
            local tmpfs_path="/dev/shm/nvme-${cluster_id}-${i}.img"
            dd if=/dev/zero of="$tmpfs_path" bs=1M count="$device_size" 2>&1 | grep -v records || {
                cluster_error "Failed to create tmpfs backing file"
                return 1
            }
            backing_device="$tmpfs_path"
        fi

        # For file/sparsefile backing, create loop device for block device support
        # (needed for NVMe persistent reservations - file backing doesn't support them)
        if [ "$backing_type" = "file" ] || [ "$backing_type" = "sparsefile" ]; then
            # For sparsefile, create the file if it doesn't exist
            if [ "$backing_type" = "sparsefile" ] && [ ! -f "$backing_device" ]; then
                cluster_debug "Creating sparse file for NVMe-oF: $backing_device"
                truncate -s "${device_size}M" "$backing_device" || {
                    cluster_error "Failed to create sparse file: $backing_device"
                    return 1
                }
                chmod 600 "$backing_device"
            fi

            # Create loop device to provide block device backing
            cluster_debug "Creating loop device for NVMe namespace backing: $backing_device"
            local loop_dev=$(losetup -f --show "$backing_device") || {
                cluster_error "Failed to create loop device for: $backing_device"
                return 1
            }
            backing_device="$loop_dev"
            cluster_debug "Using loop device for namespace $ns_id: $loop_dev"
        fi

        # Set device path
        echo "$backing_device" > "$ns_path/device_path" || {
            cluster_error "Failed to set device_path for namespace $ns_id"
            return 1
        }

        # Enable NVMe persistent reservations (required for sanlock)
        # Must be set before enabling the namespace
        if [ -f "$ns_path/resv_enable" ]; then
            echo 1 > "$ns_path/resv_enable" || {
                cluster_warn "Failed to enable reservations for namespace $ns_id (may not be supported)"
            }
            cluster_debug "Enabled persistent reservations for namespace $ns_id"
        else
            cluster_warn "Namespace reservation support not available (kernel may not support it)"
        fi

        # Enable namespace
        echo 1 > "$ns_path/enable" || {
            cluster_error "Failed to enable namespace $ns_id"
            return 1
        }

        cluster_debug "Namespace $ns_id configured with backing: $backing_device"
    done

    # Create or configure port
    if [ ! -d "$port_path" ]; then
        cluster_log "Creating NVMe TCP port"
        mkdir -p "$port_path" || {
            cluster_error "Failed to create port directory"
            return 1
        }
    fi

    # Configure port for TCP
    echo "tcp" > "$port_path/addr_trtype" || {
        cluster_error "Failed to set transport type"
        return 1
    }

    echo "$exporter_ip" > "$port_path/addr_traddr" || {
        cluster_error "Failed to set transport address"
        return 1
    }

    echo "4420" > "$port_path/addr_trsvcid" || {
        cluster_error "Failed to set port number"
        return 1
    }

    echo "ipv4" > "$port_path/addr_adrfam" || {
        cluster_error "Failed to set address family"
        return 1
    }

    # Link subsystem to port
    cluster_log "Linking subsystem to TCP port"
    ln -s "$subsystem_path" "$port_path/subsystems/$subsystem_nqn" 2>/dev/null || {
        # Link might already exist, check if it's correct
        if [ ! -L "$port_path/subsystems/$subsystem_nqn" ]; then
            cluster_error "Failed to link subsystem to port"
            return 1
        fi
    }

    cluster_log "NVMe-oF target configuration complete"
    cluster_log "  Subsystem NQN: $subsystem_nqn"
    cluster_log "  Transport: TCP"
    cluster_log "  Address: $exporter_ip:4420"
    cluster_log "  Namespaces: $num_devices"

    return 0
}

#
# cluster_storage_cleanup - Clean up storage configuration on node 0
#
# Arguments:
#   $1 - cluster_id
#   $2 - storage_type (iscsi, nvme, or both)
#
# Returns:
#   0 on success
#
cluster_storage_cleanup() {
    local cluster_id="$1"
    local storage_type="${2:-both}"

    cluster_log "Cleaning up storage configuration (type=$storage_type)"

    # Cleanup iSCSI
    if [ "$storage_type" = "iscsi" ] || [ "$storage_type" = "both" ]; then
        local target_iqn="iqn.2025-03.com.lvm:cluster.${cluster_id}"

        if command -v targetcli &>/dev/null; then
            cluster_debug "Removing iSCSI target: $target_iqn"
            targetcli /iscsi delete "$target_iqn" 2>/dev/null || true

            # Clean up backstores
            # Note: Deletes all lvmdisk-* backstores (assumes one cluster at a time)
            for bs in $(targetcli /backstores/fileio ls 2>/dev/null | grep -o 'lvmdisk-[0-9]*' || true); do
                targetcli /backstores/fileio delete "$bs" 2>/dev/null || true
            done
            for bs in $(targetcli /backstores/ramdisk ls 2>/dev/null | grep -o 'lvmdisk-[0-9]*' || true); do
                targetcli /backstores/ramdisk delete "$bs" 2>/dev/null || true
            done

            targetcli saveconfig 2>/dev/null || true
        fi
    fi

    # Cleanup NVMe-oF
    if [ "$storage_type" = "nvme" ] || [ "$storage_type" = "both" ]; then
        local subsystem_nqn="nqn.2025-03.lvm.cluster:${cluster_id}"
        local nvmet_base="/sys/kernel/config/nvmet"
        local subsystem_path="$nvmet_base/subsystems/$subsystem_nqn"
        local port_path="$nvmet_base/ports/1"

        if [ -d "$subsystem_path" ]; then
            cluster_debug "Removing NVMe subsystem: $subsystem_nqn"

            # Remove port link
            if [ -L "$port_path/subsystems/$subsystem_nqn" ]; then
                rm -f "$port_path/subsystems/$subsystem_nqn" || true
            fi

            # Disable and remove namespaces
            for ns in "$subsystem_path"/namespaces/*; do
                if [ -d "$ns" ]; then
                    echo 0 > "$ns/enable" 2>/dev/null || true
                    rmdir "$ns" 2>/dev/null || true
                fi
            done

            # Remove subsystem
            rmdir "$subsystem_path" 2>/dev/null || true
        fi

        # Detach loop devices used for NVMe namespace backing
        cluster_debug "Detaching loop devices for NVMe namespaces"
        for loop_dev in $(losetup -j "$CLUSTER_STORAGE_DIR" 2>/dev/null | grep "$cluster_id" | cut -d: -f1 || true); do
            cluster_debug "Detaching NVMe loop device: $loop_dev"
            losetup -d "$loop_dev" 2>/dev/null || true
        done

        # Clean up tmpfs files used for ramdisk backing
        rm -f /dev/shm/nvme-${cluster_id}-*.img 2>/dev/null || true
    fi

    # Clean up backing files
    rm -rf "${CLUSTER_STORAGE_DIR}/disk-${cluster_id}-"*.img 2>/dev/null || true

    # Clean up firewall rules
    if command -v firewall-cmd &>/dev/null && systemctl is-active firewalld &>/dev/null; then
        cluster_debug "Cleaning up firewall rules"

        case "$storage_type" in
            iscsi)
                firewall-cmd --permanent --remove-port=3260/tcp 2>/dev/null || true
                ;;
            nvme)
                firewall-cmd --permanent --remove-port=4420/tcp 2>/dev/null || true
                ;;
            both)
                firewall-cmd --permanent --remove-port=3260/tcp 2>/dev/null || true
                firewall-cmd --permanent --remove-port=4420/tcp 2>/dev/null || true
                ;;
        esac

        firewall-cmd --reload 2>/dev/null || true
    fi

    cluster_log "Storage cleanup complete"
    return 0
}

#
# cluster_storage_setup - Main storage export setup orchestration
#
# This is the main entry point for setting up storage export on node 0.
# It orchestrates:
#   1. Creating backing storage
#   2. Configuring iSCSI targets (if enabled)
#   3. Configuring NVMe-oF targets (if enabled)
#
# This function should be called via SSH on node 0.
#
# Arguments:
#   $1 - cluster_id
#
# Environment variables required:
#   CLUSTER_STORAGE_TYPE (iscsi, nvme, or both)
#   CLUSTER_NUM_DEVICES
#   CLUSTER_DEVICE_SIZE
#   CLUSTER_DEVICE_SECTOR_SIZE
#   CLUSTER_BACKING_TYPE
#   NODE0_IP (IP address of node 0)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_storage_setup() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_error "cluster_id is required"
        return 1
    fi

    # Validate required environment variables
    if [ -z "${CLUSTER_STORAGE_TYPE:-}" ]; then
        cluster_error "CLUSTER_STORAGE_TYPE not set"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_DEVICES:-}" ]; then
        cluster_error "CLUSTER_NUM_DEVICES not set"
        return 1
    fi

    if [ -z "${CLUSTER_DEVICE_SIZE:-}" ]; then
        cluster_error "CLUSTER_DEVICE_SIZE not set"
        return 1
    fi

    if [ -z "${CLUSTER_BACKING_TYPE:-}" ]; then
        cluster_error "CLUSTER_BACKING_TYPE not set"
        return 1
    fi

    if [ -z "${NODE0_IP:-}" ]; then
        cluster_error "NODE0_IP not set"
        return 1
    fi

    local storage_type="${CLUSTER_STORAGE_TYPE}"
    local num_devices="${CLUSTER_NUM_DEVICES}"
    local device_size="${CLUSTER_DEVICE_SIZE}"
    local sector_size="${CLUSTER_DEVICE_SECTOR_SIZE:-512}"
    local backing_type="${CLUSTER_BACKING_TYPE}"
    local exporter_ip="${NODE0_IP}"

    cluster_log "=========================================="
    cluster_log "Storage Export Setup on Node 0"
    cluster_log "=========================================="
    cluster_log "Cluster ID: $cluster_id"
    cluster_log "Storage Type: $storage_type"
    cluster_log "Backing Type: $backing_type"
    cluster_log "Number of Devices: $num_devices"
    cluster_log "Device Size: ${device_size}MB"
    cluster_log "Sector Size: $sector_size"
    cluster_log "Exporter IP: $exporter_ip"
    cluster_log "=========================================="

    # Step 1: Create backing storage
    cluster_storage_create_backing "$backing_type" "$num_devices" "$device_size" "$sector_size" "$cluster_id" || {
        cluster_error "Failed to create backing storage"
        return 1
    }

    # Step 2: Configure storage targets based on type
    case "$storage_type" in
        iscsi)
            cluster_iscsi_setup_target "$exporter_ip" "$num_devices" "$backing_type" "$device_size" "$sector_size" "$cluster_id" || {
                cluster_error "Failed to setup iSCSI target"
                return 1
            }
            ;;

        nvme)
            cluster_nvme_setup_target "$exporter_ip" "$num_devices" "$backing_type" "$device_size" "$sector_size" "$cluster_id" || {
                cluster_error "Failed to setup NVMe-oF target"
                return 1
            }
            ;;

        both)
            cluster_iscsi_setup_target "$exporter_ip" "$num_devices" "$backing_type" "$device_size" "$sector_size" "$cluster_id" || {
                cluster_error "Failed to setup iSCSI target"
                return 1
            }

            cluster_nvme_setup_target "$exporter_ip" "$num_devices" "$backing_type" "$device_size" "$sector_size" "$cluster_id" || {
                cluster_error "Failed to setup NVMe-oF target"
                return 1
            }
            ;;

        *)
            cluster_error "Unknown storage type: $storage_type"
            return 1
            ;;
    esac

    # Step 3: Configure firewall to allow storage traffic
    cluster_log "Configuring firewall for storage access"

    if command -v firewall-cmd &>/dev/null && systemctl is-active firewalld &>/dev/null; then
        cluster_log "Firewalld is active, opening ports"

        case "$storage_type" in
            iscsi)
                cluster_log "Opening iSCSI port 3260/tcp"
                firewall-cmd --permanent --add-port=3260/tcp || cluster_warn "Failed to add iSCSI port to firewall"
                ;;
            nvme)
                cluster_log "Opening NVMe-oF port 4420/tcp"
                firewall-cmd --permanent --add-port=4420/tcp || cluster_warn "Failed to add NVMe port to firewall"
                ;;
            both)
                cluster_log "Opening iSCSI port 3260/tcp and NVMe-oF port 4420/tcp"
                firewall-cmd --permanent --add-port=3260/tcp || cluster_warn "Failed to add iSCSI port to firewall"
                firewall-cmd --permanent --add-port=4420/tcp || cluster_warn "Failed to add NVMe port to firewall"
                ;;
        esac

        firewall-cmd --reload || cluster_warn "Failed to reload firewall"
        cluster_log "Firewall configured successfully"
    else
        cluster_debug "Firewalld not active, skipping firewall configuration"
    fi

    cluster_log "=========================================="
    cluster_log "Storage export setup complete!"
    cluster_log "=========================================="

    return 0
}

# If script is executed directly (not sourced), run cluster_storage_setup
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    if [ $# -lt 1 ]; then
        echo "Usage: $0 <cluster_id>" >&2
        echo "" >&2
        echo "Required environment variables:" >&2
        echo "  CLUSTER_STORAGE_TYPE - iscsi, nvme, or both" >&2
        echo "  CLUSTER_NUM_DEVICES - number of devices to export" >&2
        echo "  CLUSTER_DEVICE_SIZE - device size in MB" >&2
        echo "  CLUSTER_BACKING_TYPE - file, sparsefile, or ramdisk" >&2
        echo "  NODE0_IP - IP address of node 0" >&2
        exit 1
    fi

    cluster_storage_setup "$1"
fi
