#!/bin/bash
#
# cluster-storage-importer.sh - Storage import layer for LVM cluster testing
#
# This script runs on test nodes (nodes 1..N) to:
# - Configure iSCSI initiators to connect to node 0
# - Configure NVMe-oF initiators to connect to node 0
# - Discover and list imported devices
# - Configure multipath if enabled
#
# Test nodes import shared storage from node 0 (storage exporter)
#

# Source the core library for logging functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-test-lib.sh" || exit 1

#
# cluster_iscsi_setup_initiator - Configure iSCSI initiator on test node
#
# Uses iscsiadm to:
#   - Discover available targets from node 0
#   - Login to the target
#   - Establish iSCSI session
#
# Arguments:
#   $1 - node_ip (IP address of this test node, for logging)
#   $2 - target_ip (IP address of node 0, the storage exporter)
#   $3 - target_iqn (iSCSI target IQN to connect to)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_iscsi_setup_initiator() {
    local node_ip="$1"
    local target_ip="$2"
    local target_iqn="$3"

    cluster_log "Configuring iSCSI initiator to connect to $target_ip"

    # Check if iscsiadm is available
    if ! command -v iscsiadm &>/dev/null; then
        cluster_error "iscsiadm command not found - is iscsi-initiator-utils package installed?"
        return 1
    fi

    # Ensure iscsid service is running
    if command -v systemctl &>/dev/null; then
        cluster_debug "Starting iscsid service"
        systemctl start iscsid 2>/dev/null || true
        systemctl enable iscsid 2>/dev/null || true
    fi

    # Discover targets from node 0
    cluster_log "Discovering iSCSI targets from $target_ip"
    iscsiadm -m discovery -t sendtargets -p "$target_ip:3260" || {
        cluster_error "Failed to discover iSCSI targets from $target_ip"
        return 1
    }

    # Login to the target
    cluster_log "Logging in to iSCSI target: $target_iqn"
    iscsiadm -m node -T "$target_iqn" -p "$target_ip:3260" --login || {
        cluster_error "Failed to login to iSCSI target: $target_iqn"
        return 1
    }

    # Wait a moment for devices to appear
    cluster_debug "Waiting for iSCSI devices to appear"
    sleep 2

    # Verify session is active
    if ! iscsiadm -m session 2>/dev/null | grep -q "$target_iqn"; then
        cluster_error "iSCSI session not established for target: $target_iqn"
        return 1
    fi

    cluster_log "iSCSI initiator configuration complete"
    cluster_log "  Target: $target_iqn"
    cluster_log "  Portal: $target_ip:3260"

    # List discovered devices
    cluster_debug "Discovered iSCSI devices:"
    lsblk -d -n -o NAME,SIZE,VENDOR,MODEL 2>/dev/null | grep -i 'LIO-ORG' || true

    return 0
}

#
# cluster_nvme_setup_initiator - Configure NVMe-oF initiator on test node
#
# Uses nvme-cli to:
#   - Discover NVMe subsystems from node 0
#   - Connect to the NVMe-oF target
#   - Establish NVMe-oF connection
#
# Arguments:
#   $1 - node_ip (IP address of this test node, for logging)
#   $2 - target_ip (IP address of node 0, the storage exporter)
#   $3 - subsystem_nqn (NVMe subsystem NQN to connect to)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_nvme_setup_initiator() {
    local node_ip="$1"
    local target_ip="$2"
    local subsystem_nqn="$3"

    cluster_log "Configuring NVMe-oF initiator to connect to $target_ip"

    # Check if nvme command is available
    if ! command -v nvme &>/dev/null; then
        cluster_error "nvme command not found - is nvme-cli package installed?"
        return 1
    fi

    # Load nvme-tcp kernel module if needed
    if ! lsmod | grep -q nvme_tcp; then
        cluster_log "Loading nvme-tcp kernel module"
        modprobe nvme-tcp || {
            cluster_error "Failed to load nvme-tcp module"
            return 1
        }
    fi

    # Discover NVMe subsystems from node 0
    cluster_log "Discovering NVMe subsystems from $target_ip"
    nvme discover -t tcp -a "$target_ip" -s 4420 || {
        cluster_warn "NVMe discovery failed or returned no results"
    }

    # Connect to the NVMe-oF target
    cluster_log "Connecting to NVMe subsystem: $subsystem_nqn"
    local connect_output
    connect_output=$(nvme connect -t tcp -n "$subsystem_nqn" -a "$target_ip" -s 4420 2>&1)
    local connect_status=$?

    echo "$connect_output"

    if [ $connect_status -ne 0 ]; then
        cluster_error "Failed to connect to NVMe subsystem: $subsystem_nqn (exit code: $connect_status)"
        cluster_error "Error output: $connect_output"
        return 1
    fi

    # Wait a moment for devices to appear
    cluster_debug "Waiting for NVMe devices to appear"
    sleep 3

    # Verify connection is active - check for nvme devices
    cluster_debug "Checking for NVMe controller devices"
    if ! ls /dev/nvme* >/dev/null 2>&1; then
        cluster_error "No NVMe devices found after connection attempt"
        cluster_debug "NVMe subsystem list:"
        nvme list-subsys 2>&1 || true
        return 1
    fi

    # Check if the controller is connected
    if ! nvme list-subsys 2>/dev/null | grep -q "$subsystem_nqn"; then
        cluster_warn "Subsystem NQN not found in list, but NVMe devices exist"
        cluster_debug "Available NVMe devices:"
        nvme list 2>&1 || true
    fi

    cluster_log "NVMe-oF initiator configuration complete"
    cluster_log "  Subsystem: $subsystem_nqn"
    cluster_log "  Transport: TCP"
    cluster_log "  Address: $target_ip:4420"

    # List discovered devices
    cluster_debug "Discovered NVMe devices:"
    nvme list 2>/dev/null | grep -E 'Node|nvme' || true

    return 0
}

#
# cluster_multipath_setup - Configure device-mapper multipath
#
# Sets up multipath configuration for redundant paths to storage.
# This is used when CLUSTER_MULTIPATH_ENABLE=1.
#
# Arguments:
#   $1 - node_ip (IP address of this test node)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_multipath_setup() {
    local node_ip="$1"

    cluster_log "Configuring device-mapper multipath"

    # Check if multipath tools are available
    if ! command -v multipath &>/dev/null; then
        cluster_error "multipath command not found - is device-mapper-multipath package installed?"
        return 1
    fi

    # Create basic multipath configuration
    local multipath_conf="/etc/multipath.conf"
    local backup_conf="${multipath_conf}.backup-$(date +%s)"

    # Backup existing config if present
    if [ -f "$multipath_conf" ]; then
        cluster_debug "Backing up existing multipath config to: $backup_conf"
        cp "$multipath_conf" "$backup_conf"
    fi

    cluster_log "Creating multipath configuration"
    cat > "$multipath_conf" <<'EOF'
# LVM Cluster Testing Multipath Configuration
# Auto-generated by cluster-storage-importer.sh

defaults {
    user_friendly_names yes
    find_multipaths yes
    polling_interval 10
}

# Blacklist local devices
blacklist {
    devnode "^(ram|raw|loop|fd|md|dm-|sr|scd|st)[0-9]*"
    devnode "^hd[a-z]"
    devnode "^vd[a-z]"
}

# Allow iSCSI and NVMe devices
blacklist_exceptions {
    property "(SCSI_IDENT_|ID_WWN)"
}
EOF

    # Reload multipath configuration
    cluster_log "Reloading multipath configuration"
    if command -v systemctl &>/dev/null; then
        systemctl restart multipathd || {
            cluster_error "Failed to restart multipathd service"
            return 1
        }
        systemctl enable multipathd 2>/dev/null || true
    else
        service multipathd restart || {
            cluster_error "Failed to restart multipathd service"
            return 1
        }
    fi

    # Wait for multipath to scan devices
    sleep 2

    # Reconfigure multipath devices
    multipath -r || {
        cluster_warn "Failed to reconfigure multipath devices"
    }

    cluster_log "Multipath configuration complete"

    # Show multipath status
    cluster_debug "Multipath device status:"
    multipath -ll 2>/dev/null || true

    return 0
}

#
# cluster_storage_discover_devices - Discover imported storage devices
#
# Scans for devices that were imported via iSCSI or NVMe-oF and
# lists them for use in tests.
#
# Arguments:
#   $1 - storage_type (iscsi, nvme, or both)
#   $2 - multipath_enabled (0 or 1)
#
# Outputs:
#   List of device paths (one per line) to stdout
#
# Returns:
#   0 on success, 1 on failure
#
cluster_storage_discover_devices() {
    local storage_type="$1"
    local multipath_enabled="${2:-0}"

    cluster_log "Discovering imported storage devices (type=$storage_type, multipath=$multipath_enabled)"

    # Rescan SCSI bus to ensure all devices are visible
    if [ "$storage_type" = "iscsi" ] || [ "$storage_type" = "both" ]; then
        cluster_debug "Rescanning SCSI bus"
        if [ -f /usr/bin/rescan-scsi-bus.sh ]; then
            /usr/bin/rescan-scsi-bus.sh -a 2>/dev/null || true
        elif command -v iscsiadm &>/dev/null; then
            iscsiadm -m session --rescan 2>/dev/null || true
        fi
    fi

    # Rescan NVMe devices
    if [ "$storage_type" = "nvme" ] || [ "$storage_type" = "both" ]; then
        cluster_debug "Rescanning NVMe devices"
        if command -v nvme &>/dev/null; then
            # Force kernel to rescan namespaces
            for ctrl in /sys/class/nvme/nvme*/; do
                [ -d "$ctrl" ] || continue
                echo 1 > "${ctrl}/rescan_controller" 2>/dev/null || true
            done
        fi
    fi

    # Wait for udev to settle
    if command -v udevadm &>/dev/null; then
        cluster_debug "Waiting for udev to settle"
        udevadm settle --timeout=10 2>/dev/null || true
    fi

    local devices=()

    if [ "$multipath_enabled" = "1" ]; then
        # Use multipath devices
        cluster_debug "Discovering multipath devices"

        # Get multipath device names
        while IFS= read -r mpath_dev; do
            if [ -n "$mpath_dev" ] && [ -b "/dev/mapper/$mpath_dev" ]; then
                devices+=("/dev/mapper/$mpath_dev")
            fi
        done < <(multipath -ll 2>/dev/null | grep -oP '^[a-z0-9]+(?= \()' || true)

    else
        # Discover raw devices
        case "$storage_type" in
            iscsi)
                # iSCSI devices are typically /dev/sd* from LIO-ORG
                cluster_debug "Discovering iSCSI devices (looking for LIO-ORG vendor)"
                while IFS= read -r dev; do
                    if [ -n "$dev" ] && [ -b "/dev/$dev" ]; then
                        devices+=("/dev/$dev")
                    fi
                done < <(lsblk -d -n -o NAME,VENDOR 2>/dev/null | grep -i 'LIO-ORG' | awk '{print $1}' || true)
                ;;

            nvme)
                # NVMe devices are /dev/nvme*n*
                cluster_debug "Discovering NVMe devices"
                while IFS= read -r dev; do
                    if [ -n "$dev" ] && [ -b "$dev" ]; then
                        devices+=("$dev")
                    fi
                done < <(nvme list 2>/dev/null | grep -oP '/dev/nvme[0-9]+n[0-9]+' || true)
                ;;

            both)
                # Discover both iSCSI and NVMe devices
                cluster_debug "Discovering both iSCSI and NVMe devices"

                # iSCSI devices
                while IFS= read -r dev; do
                    if [ -n "$dev" ] && [ -b "/dev/$dev" ]; then
                        devices+=("/dev/$dev")
                    fi
                done < <(lsblk -d -n -o NAME,VENDOR 2>/dev/null | grep -i 'LIO-ORG' | awk '{print $1}' || true)

                # NVMe devices
                while IFS= read -r dev; do
                    if [ -n "$dev" ] && [ -b "$dev" ]; then
                        devices+=("$dev")
                    fi
                done < <(nvme list 2>/dev/null | grep -oP '/dev/nvme[0-9]+n[0-9]+' || true)
                ;;
        esac
    fi

    # Output discovered devices
    if [ ${#devices[@]} -eq 0 ]; then
        cluster_warn "No storage devices discovered"
        return 1
    fi

    cluster_log "Discovered ${#devices[@]} storage device(s):"
    for dev in "${devices[@]}"; do
        cluster_log "  - $dev"
        echo "$dev"
    done

    return 0
}

#
# cluster_storage_import_disconnect - Disconnect from imported storage
#
# Disconnects iSCSI sessions and NVMe-oF connections.
# Used during cluster cleanup.
#
# Arguments:
#   $1 - storage_type (iscsi, nvme, or both)
#   $2 - target_iqn (for iSCSI, optional)
#   $3 - subsystem_nqn (for NVMe, optional)
#
# Returns:
#   0 on success
#
cluster_storage_import_disconnect() {
    local storage_type="$1"
    local target_iqn="${2:-}"
    local subsystem_nqn="${3:-}"

    cluster_log "Disconnecting from imported storage (type=$storage_type)"

    # Disconnect iSCSI
    if [ "$storage_type" = "iscsi" ] || [ "$storage_type" = "both" ]; then
        if command -v iscsiadm &>/dev/null; then
            if [ -n "$target_iqn" ]; then
                cluster_debug "Logging out of iSCSI target: $target_iqn"
                iscsiadm -m node -T "$target_iqn" --logout 2>/dev/null || true
                iscsiadm -m node -T "$target_iqn" --op delete 2>/dev/null || true
            else
                cluster_debug "Logging out of all iSCSI targets"
                iscsiadm -m node --logoutall=all 2>/dev/null || true
            fi
        fi
    fi

    # Disconnect NVMe-oF
    if [ "$storage_type" = "nvme" ] || [ "$storage_type" = "both" ]; then
        if command -v nvme &>/dev/null; then
            if [ -n "$subsystem_nqn" ]; then
                cluster_debug "Disconnecting from NVMe subsystem: $subsystem_nqn"
                nvme disconnect -n "$subsystem_nqn" 2>/dev/null || true
            else
                cluster_debug "Disconnecting all NVMe-oF connections"
                nvme disconnect-all 2>/dev/null || true
            fi
        fi
    fi

    # Wait for devices to be removed
    sleep 1

    cluster_log "Storage disconnect complete"
    return 0
}

#
# cluster_storage_import - Main storage import orchestration
#
# This is the main entry point for setting up storage import on a test node.
# It orchestrates:
#   1. Configuring iSCSI initiator (if enabled)
#   2. Configuring NVMe-oF initiator (if enabled)
#   3. Configuring multipath (if enabled)
#   4. Discovering imported devices
#
# This function should be called via SSH on test nodes (1..N).
#
# Arguments:
#   $1 - cluster_id
#   $2 - node_num (this node's number: 1, 2, 3, ...)
#
# Environment variables required:
#   CLUSTER_STORAGE_TYPE (iscsi, nvme, or both)
#   CLUSTER_MULTIPATH_ENABLE (0 or 1)
#   NODE0_IP (IP address of node 0, the storage exporter)
#   THIS_NODE_IP (IP address of this test node)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_storage_import() {
    local cluster_id="$1"
    local node_num="$2"

    if [ -z "$cluster_id" ]; then
        cluster_error "cluster_id is required"
        return 1
    fi

    if [ -z "$node_num" ]; then
        cluster_error "node_num is required"
        return 1
    fi

    # Validate required environment variables
    if [ -z "${CLUSTER_STORAGE_TYPE:-}" ]; then
        cluster_error "CLUSTER_STORAGE_TYPE not set"
        return 1
    fi

    if [ -z "${NODE0_IP:-}" ]; then
        cluster_error "NODE0_IP not set"
        return 1
    fi

    if [ -z "${THIS_NODE_IP:-}" ]; then
        cluster_error "THIS_NODE_IP not set"
        return 1
    fi

    local storage_type="${CLUSTER_STORAGE_TYPE}"
    local multipath_enabled="${CLUSTER_MULTIPATH_ENABLE:-0}"
    local target_ip="${NODE0_IP}"
    local node_ip="${THIS_NODE_IP}"

    # Construct target identifiers
    local target_iqn="iqn.2025-03.com.lvm:cluster.${cluster_id}"
    local subsystem_nqn="nqn.2025-03.lvm.cluster:${cluster_id}"

    cluster_log "=========================================="
    cluster_log "Storage Import Setup on Node $node_num"
    cluster_log "=========================================="
    cluster_log "Cluster ID: $cluster_id"
    cluster_log "Storage Type: $storage_type"
    cluster_log "Multipath Enabled: $multipath_enabled"
    cluster_log "Node IP: $node_ip"
    cluster_log "Target IP (Node 0): $target_ip"
    cluster_log "=========================================="

    # Step 1: Configure multipath if enabled (must be before initiator setup)
    if [ "$multipath_enabled" = "1" ]; then
        cluster_multipath_setup "$node_ip" || {
            cluster_error "Failed to setup multipath"
            return 1
        }
    fi

    # Step 2: Configure initiators based on storage type
    case "$storage_type" in
        iscsi)
            cluster_iscsi_setup_initiator "$node_ip" "$target_ip" "$target_iqn" || {
                cluster_error "Failed to setup iSCSI initiator"
                return 1
            }
            ;;

        nvme)
            cluster_nvme_setup_initiator "$node_ip" "$target_ip" "$subsystem_nqn" || {
                cluster_error "Failed to setup NVMe-oF initiator"
                return 1
            }
            ;;

        both)
            cluster_iscsi_setup_initiator "$node_ip" "$target_ip" "$target_iqn" || {
                cluster_error "Failed to setup iSCSI initiator"
                return 1
            }

            cluster_nvme_setup_initiator "$node_ip" "$target_ip" "$subsystem_nqn" || {
                cluster_error "Failed to setup NVMe-oF initiator"
                return 1
            }
            ;;

        *)
            cluster_error "Unknown storage type: $storage_type"
            return 1
            ;;
    esac

    # Step 3: Discover imported devices
    cluster_storage_discover_devices "$storage_type" "$multipath_enabled" || {
        cluster_error "Failed to discover storage devices"
        return 1
    }

    cluster_log "=========================================="
    cluster_log "Storage import setup complete on node $node_num!"
    cluster_log "=========================================="

    return 0
}

# If script is executed directly (not sourced), run cluster_storage_import
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    if [ $# -lt 2 ]; then
        echo "Usage: $0 <cluster_id> <node_num>" >&2
        echo "" >&2
        echo "Required environment variables:" >&2
        echo "  CLUSTER_STORAGE_TYPE - iscsi, nvme, or both" >&2
        echo "  CLUSTER_MULTIPATH_ENABLE - 0 or 1" >&2
        echo "  NODE0_IP - IP address of node 0 (storage exporter)" >&2
        echo "  THIS_NODE_IP - IP address of this test node" >&2
        exit 1
    fi

    cluster_storage_import "$1" "$2"
fi
