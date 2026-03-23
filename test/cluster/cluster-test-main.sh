#!/bin/bash
#
# cluster-test-main.sh - Main orchestration script for LVM cluster testing
#
# Usage:
#   cluster-test-main.sh [-c config] [-i cluster_id] <command>
#
# Commands:
#   create   - Create a new cluster
#   destroy  - Destroy an existing cluster
#   status   - Show cluster status
#   list     - List all clusters
#
# Examples:
#   # Create cluster with default config
#   ./cluster-test-main.sh create
#
#   # Create cluster with custom config
#   ./cluster-test-main.sh -c configs/my-cluster.conf create
#
#   # Destroy specific cluster
#   ./cluster-test-main.sh -i lvmtest-12345-67890 destroy
#
#   # Show cluster status
#   ./cluster-test-main.sh -i lvmtest-12345-67890 status
#

set -e

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source required libraries
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-test-lib.sh"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-vm-manager.sh"

#
# Usage and help
#

usage() {
    cat <<EOF
Usage: $0 [-c config] [-i cluster_id] <command>

Commands:
  create   - Create a new cluster
  destroy  - Destroy an existing cluster
  status   - Show cluster status
  list     - List all clusters

Options:
  -c config      Configuration file (default: configs/default-cluster.conf)
  -i cluster_id  Cluster ID for operations (optional if only one cluster exists)
  -h             Show this help message

Examples:
  # Create cluster with default config
  $0 create

  # Create cluster with custom config
  $0 -c configs/my-cluster.conf create

  # Destroy cluster (auto-detects if only one exists)
  $0 destroy

  # Destroy specific cluster (when multiple exist)
  $0 -i lvmtest-12345-67890 destroy

  # Show cluster status (auto-detects if only one exists)
  $0 status

  # List all clusters
  $0 list

EOF
    exit 1
}

#
# Helper functions
#

cluster_auto_detect_id() {
    local command="$1"

    # Get list of available clusters
    local clusters=($(cluster_state_list))

    if [ ${#clusters[@]} -eq 0 ]; then
        # Check for orphaned VMs
        local orphaned_vms=$(virsh list --all 2>/dev/null | grep lvmtest | awk '{print $2}' || true)
        if [ -n "$orphaned_vms" ]; then
            cluster_error "No cluster state files found, but found VMs without state (orphaned):"
            echo "$orphaned_vms" | while read -r vm_name; do
                cluster_error "  $vm_name"
            done
            cluster_error ""
            cluster_error "These VMs can be listed with: $0 list"
            cluster_error "Or manually destroyed with virsh destroy/undefine commands"
            cluster_die "Cannot proceed without cluster state file"
        else
            cluster_die "No clusters found. Please create a cluster first with: $0 create"
        fi
    elif [ ${#clusters[@]} -eq 1 ]; then
        # Only one cluster exists, use it
        local detected_id="${clusters[0]}"
        cluster_log "Auto-detected cluster: $detected_id"
        echo "$detected_id"
    else
        # Multiple clusters exist, require explicit selection
        cluster_error "Multiple clusters found. Please specify cluster ID with -i option:"
        for cluster_id in "${clusters[@]}"; do
            cluster_error "  $cluster_id"
        done
        cluster_die "Use: $0 -i <cluster-id> $command"
    fi
}

#
# Command implementations
#

cmd_create() {
    cluster_log "Creating new cluster"

    # Generate cluster ID
    local cluster_id=$(cluster_generate_id)
    export CLUSTER_ID="$cluster_id"

    cluster_log "Cluster ID: $cluster_id"

    # Create all VMs
    cluster_vms_create_all "$cluster_id"

    # Save state
    cluster_state_save "$cluster_id"

    cluster_log ""
    cluster_log "Cluster created successfully!"
    cluster_log "Cluster ID: $cluster_id"
    cluster_log ""
    cluster_log "Node IPs:"
    for i in "${!CLUSTER_NODE_IPS[@]}"; do
        cluster_log "  Node $i: ${CLUSTER_NODE_IPS[$i]}"
    done
    cluster_log ""
    cluster_log "To destroy this cluster, run:"
    cluster_log "  $0 -i $cluster_id destroy"
}

cmd_destroy() {
    # Auto-detect cluster ID if not provided
    if [ -z "${CLUSTER_ID:-}" ]; then
        CLUSTER_ID=$(cluster_auto_detect_id "destroy")
    fi

    cluster_log "Destroying cluster: $CLUSTER_ID"

    # Load cluster state
    cluster_state_load "$CLUSTER_ID"

    # Destroy all VMs
    cluster_vms_destroy_all "$CLUSTER_ID"

    # Delete state
    cluster_state_delete "$CLUSTER_ID"

    cluster_log "Cluster destroyed successfully: $CLUSTER_ID"
}

cmd_status() {
    # Auto-detect cluster ID if not provided
    if [ -z "${CLUSTER_ID:-}" ]; then
        CLUSTER_ID=$(cluster_auto_detect_id "status")
    fi

    cluster_log "Cluster status: $CLUSTER_ID"

    # Load cluster state
    cluster_state_load "$CLUSTER_ID"

    echo ""
    echo "Cluster ID: $CLUSTER_ID"
    echo "Number of test nodes: ${CLUSTER_NUM_NODES:-unknown}"
    echo "Storage type: ${CLUSTER_STORAGE_TYPE:-unknown}"
    echo "Lock type: ${CLUSTER_LOCK_TYPE:-unknown}"
    echo ""

    if [ -n "${CLUSTER_NODE_IPS:-}" ]; then
        echo "Node IPs:"
        for i in "${!CLUSTER_NODE_IPS[@]}"; do
            local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$i")
            local state="unknown"

            if virsh dominfo "$vm_name" &>/dev/null; then
                state=$(virsh domstate "$vm_name" 2>/dev/null || echo "unknown")
            else
                state="not found"
            fi

            printf "  Node %d: %-15s (%s)\n" "$i" "${CLUSTER_NODE_IPS[$i]}" "$state"
        done
    else
        echo "No node IPs found in state"
    fi

    echo ""
}

cmd_list() {
    cluster_log "Listing all clusters"
    echo ""

    local clusters=($(cluster_state_list))

    if [ ${#clusters[@]} -eq 0 ]; then
        echo "No clusters found"

        # Check for orphaned VMs (running without state files)
        local orphaned_vms=$(virsh list --all 2>/dev/null | grep lvmtest | awk '{print $2}' || true)
        if [ -n "$orphaned_vms" ]; then
            echo ""
            echo "WARNING: Found VMs without cluster state files (orphaned):"
            echo "$orphaned_vms" | while read -r vm_name; do
                echo "  $vm_name"
            done
            echo ""
            echo "These VMs can be manually destroyed with:"
            echo "  virsh destroy <vm-name>"
            echo "  virsh undefine <vm-name> --remove-all-storage"
        fi

        echo ""
        return
    fi

    echo "Available clusters:"
    for cluster_id in "${clusters[@]}"; do
        # Load state to get info
        CLUSTER_ID="$cluster_id"
        cluster_state_load "$cluster_id" 2>/dev/null || continue

        local num_nodes="${CLUSTER_NUM_NODES:-?}"
        local storage="${CLUSTER_STORAGE_TYPE:-?}"
        local lock="${CLUSTER_LOCK_TYPE:-?}"

        printf "  %-30s  nodes=%s  storage=%s  lock=%s\n" \
            "$cluster_id" "$num_nodes" "$storage" "$lock"
    done
    echo ""
}

#
# Main
#

main() {
    local config_file=""
    local cluster_id=""
    local command=""

    # Check if running as root
    cluster_check_root

    # Check dependencies
    cluster_check_deps

    # Ensure state directory exists
    mkdir -p "$CLUSTER_STATE_DIR" 2>/dev/null || true

    # Parse options
    while getopts "c:i:h" opt; do
        case $opt in
            c)
                config_file="$OPTARG"
                ;;
            i)
                cluster_id="$OPTARG"
                export CLUSTER_ID="$cluster_id"
                ;;
            h)
                usage
                ;;
            *)
                usage
                ;;
        esac
    done

    shift $((OPTIND - 1))

    # Get command
    command="${1:-}"

    if [ -z "$command" ]; then
        cluster_error "No command specified"
        usage
    fi

    # Load configuration for create command
    if [ "$command" = "create" ]; then
        if [ -z "$config_file" ]; then
            config_file="$SCRIPT_DIR/configs/default-cluster.conf"
        fi

        # Make config file path absolute
        if [[ ! "$config_file" = /* ]]; then
            config_file="$SCRIPT_DIR/$config_file"
        fi

        cluster_load_config "$config_file"
    fi

    # Execute command
    case "$command" in
        create)
            cmd_create
            ;;
        destroy)
            cmd_destroy
            ;;
        status)
            cmd_status
            ;;
        list)
            cmd_list
            ;;
        *)
            cluster_error "Unknown command: $command"
            usage
            ;;
    esac
}

# Run main
main "$@"
