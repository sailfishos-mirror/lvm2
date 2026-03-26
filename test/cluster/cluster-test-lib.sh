#!/bin/bash
#
# cluster-test-lib.sh - Core utility functions for LVM cluster testing
#
# Provides:
# - Logging utilities (cluster_log, cluster_warn, cluster_die)
# - Configuration loading and validation (cluster_load_config)
# - Cluster ID generation (cluster_generate_id)
# - State persistence (cluster_state_save, cluster_state_load)
#

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Global state directory
CLUSTER_STATE_DIR="${CLUSTER_STATE_DIR:-/var/tmp/lvm-cluster-state}"

#
# Logging functions
#

cluster_log() {
    echo -e "${GREEN}[INFO]${NC} $*" >&2
}

cluster_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

cluster_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

cluster_die() {
    cluster_error "$@"
    exit 1
}

cluster_debug() {
    if [ "${CLUSTER_DEBUG:-0}" = "1" ]; then
        echo -e "${BLUE}[DEBUG]${NC} $*" >&2
    fi
}

#
# Configuration management
#

cluster_load_config() {
    local config_file="$1"

    if [ -z "$config_file" ]; then
        cluster_die "No configuration file specified"
    fi

    if [ ! -f "$config_file" ]; then
        cluster_die "Configuration file not found: $config_file"
    fi

    # Get the directory where this script lives to find default config
    local script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local default_config="$script_dir/configs/default-cluster.conf"

    # Load default config first (unless we're already loading it)
    if [ "$config_file" != "$default_config" ] && [ -f "$default_config" ]; then
        cluster_debug "Loading default configuration from: $default_config"
        # shellcheck disable=SC1090
        source "$default_config" || cluster_die "Failed to load default config: $default_config"
    fi

    cluster_log "Loading configuration from: $config_file"

    # Source the user config file (overwrites defaults)
    # shellcheck disable=SC1090
    source "$config_file" || cluster_die "Failed to load config file: $config_file"

    # Validate required configuration
    cluster_validate_config

    cluster_debug "Configuration loaded successfully"
}

cluster_validate_config() {
    local errors=0

    # Validate CLUSTER_NUM_NODES
    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES is not set"
        errors=$((errors + 1))
    elif [ "$CLUSTER_NUM_NODES" -lt 1 ]; then
        cluster_error "CLUSTER_NUM_NODES must be at least 1 (got: $CLUSTER_NUM_NODES)"
        errors=$((errors + 1))
    fi

    # Validate CLUSTER_NODE_MEMORY
    if [ -z "${CLUSTER_NODE_MEMORY:-}" ]; then
        cluster_error "CLUSTER_NODE_MEMORY is not set"
        errors=$((errors + 1))
    fi

    # Validate CLUSTER_NODE_VCPUS
    if [ -z "${CLUSTER_NODE_VCPUS:-}" ]; then
        cluster_error "CLUSTER_NODE_VCPUS is not set"
        errors=$((errors + 1))
    fi

    # Validate CLUSTER_STORAGE_TYPE
    if [ -z "${CLUSTER_STORAGE_TYPE:-}" ]; then
        cluster_error "CLUSTER_STORAGE_TYPE is not set"
        errors=$((errors + 1))
    elif [[ ! "$CLUSTER_STORAGE_TYPE" =~ ^(iscsi|nvme|both)$ ]]; then
        cluster_error "CLUSTER_STORAGE_TYPE must be 'iscsi', 'nvme', or 'both' (got: $CLUSTER_STORAGE_TYPE)"
        errors=$((errors + 1))
    fi

    # Validate CLUSTER_BACKING_TYPE
    if [ -z "${CLUSTER_BACKING_TYPE:-}" ]; then
        cluster_error "CLUSTER_BACKING_TYPE is not set"
        errors=$((errors + 1))
    elif [[ ! "$CLUSTER_BACKING_TYPE" =~ ^(file|sparsefile|ramdisk)$ ]]; then
        cluster_error "CLUSTER_BACKING_TYPE must be 'file', 'sparsefile', or 'ramdisk' (got: $CLUSTER_BACKING_TYPE)"
        errors=$((errors + 1))
    fi

    # Validate CLUSTER_LOCK_TYPE
    if [ -z "${CLUSTER_LOCK_TYPE:-}" ]; then
        cluster_error "CLUSTER_LOCK_TYPE is not set"
        errors=$((errors + 1))
    elif [[ ! "$CLUSTER_LOCK_TYPE" =~ ^(sanlock|dlm)$ ]]; then
        cluster_error "CLUSTER_LOCK_TYPE must be 'sanlock' or 'dlm' (got: $CLUSTER_LOCK_TYPE)"
        errors=$((errors + 1))
    fi

    # Validate source deployment settings
    if [ "${CLUSTER_USE_SOURCE:-0}" = "1" ]; then
        if [ -z "${CLUSTER_SOURCE_DIR:-}" ]; then
            # Auto-detect from current directory
            if [ -f "$(pwd)/configure.ac" ] && [ -f "$(pwd)/VERSION" ]; then
                export CLUSTER_SOURCE_DIR="$(pwd)"
                cluster_log "Auto-detected LVM source directory: $CLUSTER_SOURCE_DIR"
            else
                cluster_error "CLUSTER_USE_SOURCE=1 but CLUSTER_SOURCE_DIR is not set and auto-detection failed"
                errors=$((errors + 1))
            fi
        elif [ ! -d "$CLUSTER_SOURCE_DIR" ]; then
            cluster_error "CLUSTER_SOURCE_DIR does not exist: $CLUSTER_SOURCE_DIR"
            errors=$((errors + 1))
        fi
    fi

    # Validate sanlock source deployment settings
    if [ "${SANLOCK_USE_SOURCE:-0}" = "1" ]; then
        if [ "$CLUSTER_LOCK_TYPE" != "sanlock" ]; then
            cluster_error "SANLOCK_USE_SOURCE=1 requires CLUSTER_LOCK_TYPE=sanlock"
            errors=$((errors + 1))
        fi

        if [ -z "${SANLOCK_SOURCE_DIR:-}" ]; then
            cluster_error "SANLOCK_USE_SOURCE=1 but SANLOCK_SOURCE_DIR is not set"
            errors=$((errors + 1))
        elif [ ! -d "$SANLOCK_SOURCE_DIR" ]; then
            cluster_error "SANLOCK_SOURCE_DIR does not exist: $SANLOCK_SOURCE_DIR"
            errors=$((errors + 1))
        fi
    fi

    if [ $errors -gt 0 ]; then
        cluster_die "Configuration validation failed with $errors error(s)"
    fi

    return 0
}

#
# Cluster ID management
#

cluster_generate_id() {
    local timestamp=$(date +%s)
    local random=$(head -c 4 /dev/urandom | od -A n -t u4 | tr -d ' ')
    echo "lvmtest-${timestamp}-${random}"
}

#
# State persistence
#

cluster_state_get_file() {
    local cluster_id="$1"
    echo "${CLUSTER_STATE_DIR}/${cluster_id}.state"
}

cluster_state_save() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_die "cluster_state_save: cluster_id is required"
    fi

    # Create state directory if it doesn't exist
    mkdir -p "$CLUSTER_STATE_DIR" || cluster_die "Failed to create state directory: $CLUSTER_STATE_DIR"

    local state_file=$(cluster_state_get_file "$cluster_id")

    cluster_debug "Saving cluster state to: $state_file"

    # Save all CLUSTER_* variables to state file
    {
        echo "# Cluster state file"
        echo "# Generated: $(date)"
        echo "CLUSTER_ID=\"$cluster_id\""

        # Explicitly save important configuration variables
        # (using explicit list to ensure they're saved even if not exported)
        [ -n "${CLUSTER_NUM_NODES:-}" ] && echo "CLUSTER_NUM_NODES=$CLUSTER_NUM_NODES"
        [ -n "${CLUSTER_STORAGE_TYPE:-}" ] && echo "CLUSTER_STORAGE_TYPE=\"$CLUSTER_STORAGE_TYPE\""
        [ -n "${CLUSTER_NUM_DEVICES:-}" ] && echo "CLUSTER_NUM_DEVICES=$CLUSTER_NUM_DEVICES"
        [ -n "${CLUSTER_DEVICE_SIZE:-}" ] && echo "CLUSTER_DEVICE_SIZE=$CLUSTER_DEVICE_SIZE"
        [ -n "${CLUSTER_BACKING_TYPE:-}" ] && echo "CLUSTER_BACKING_TYPE=\"$CLUSTER_BACKING_TYPE\""
        [ -n "${CLUSTER_MULTIPATH_ENABLE:-}" ] && echo "CLUSTER_MULTIPATH_ENABLE=$CLUSTER_MULTIPATH_ENABLE"
        [ -n "${CLUSTER_LOCK_TYPE:-}" ] && echo "CLUSTER_LOCK_TYPE=\"$CLUSTER_LOCK_TYPE\""
        [ -n "${CLUSTER_SSH_KEY_DIR:-}" ] && echo "CLUSTER_SSH_KEY_DIR=\"$CLUSTER_SSH_KEY_DIR\""
        [ -n "${CLUSTER_SSH_USER:-}" ] && echo "CLUSTER_SSH_USER=\"$CLUSTER_SSH_USER\""
        [ -n "${CLUSTER_USE_SOURCE:-}" ] && echo "CLUSTER_USE_SOURCE=$CLUSTER_USE_SOURCE"
        [ -n "${SANLOCK_USE_SOURCE:-}" ] && echo "SANLOCK_USE_SOURCE=$SANLOCK_USE_SOURCE"

        # Also save any other CLUSTER_* variables that are exported
        env | grep '^CLUSTER_' | while IFS='=' read -r key value; do
            # Skip ones we already saved explicitly
            case "$key" in
                CLUSTER_ID|CLUSTER_NUM_NODES|CLUSTER_STORAGE_TYPE|CLUSTER_NUM_DEVICES|\
                CLUSTER_DEVICE_SIZE|CLUSTER_BACKING_TYPE|CLUSTER_MULTIPATH_ENABLE|\
                CLUSTER_LOCK_TYPE|CLUSTER_SSH_KEY_DIR|CLUSTER_SSH_USER|CLUSTER_USE_SOURCE)
                    continue
                    ;;
            esac
            printf '%s=%q\n' "$key" "$value"
        done

        # Export SANLOCK_* variables
        env | grep '^SANLOCK_' | while IFS='=' read -r key value; do
            case "$key" in
                SANLOCK_USE_SOURCE)
                    continue
                    ;;
            esac
            printf '%s=%q\n' "$key" "$value"
        done

        # Save node IPs if they exist
        if [ -n "${CLUSTER_NODE_IPS:-}" ]; then
            echo "CLUSTER_NODE_IPS=(${CLUSTER_NODE_IPS[*]})"
        fi

    } > "$state_file" || cluster_die "Failed to write state file: $state_file"

    cluster_debug "State saved successfully"
}

cluster_state_load() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_die "cluster_state_load: cluster_id is required"
    fi

    local state_file=$(cluster_state_get_file "$cluster_id")

    if [ ! -f "$state_file" ]; then
        cluster_die "Cluster state file not found: $state_file"
    fi

    cluster_debug "Loading cluster state from: $state_file"

    # shellcheck disable=SC1090
    source "$state_file" || cluster_die "Failed to load state file: $state_file"

    cluster_debug "State loaded successfully for cluster: $cluster_id"
}

cluster_state_delete() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_die "cluster_state_delete: cluster_id is required"
    fi

    local state_file=$(cluster_state_get_file "$cluster_id")

    if [ -f "$state_file" ]; then
        cluster_debug "Deleting state file: $state_file"
        rm -f "$state_file" || cluster_warn "Failed to delete state file: $state_file"
    fi
}

cluster_state_list() {
    if [ ! -d "$CLUSTER_STATE_DIR" ]; then
        return
    fi

    find "$CLUSTER_STATE_DIR" -name "*.state" -type f 2>/dev/null | while read -r state_file; do
        local cluster_id=$(basename "$state_file" .state)
        echo "$cluster_id"
    done
}

#
# Helper functions
#

cluster_check_root() {
    if [ "$EUID" -ne 0 ]; then
        cluster_die "This script must be run as root"
    fi
}

cluster_check_deps() {
    local deps=(virsh virt-install ssh-keygen)
    local missing=()

    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &>/dev/null; then
            missing+=("$dep")
        fi
    done

    if [ ${#missing[@]} -gt 0 ]; then
        cluster_error "Missing required dependencies: ${missing[*]}"
        cluster_die "Please install the required packages"
    fi
}

#
# Utility functions
#

cluster_wait_with_timeout() {
    local timeout="$1"
    local interval="${2:-5}"
    local check_cmd="$3"
    local description="${4:-condition}"

    local elapsed=0

    cluster_log "Waiting for $description (timeout: ${timeout}s)"

    while [ $elapsed -lt "$timeout" ]; do
        if eval "$check_cmd"; then
            cluster_log "$description ready after ${elapsed}s"
            return 0
        fi

        sleep "$interval"
        elapsed=$((elapsed + interval))

        if [ $((elapsed % 30)) -eq 0 ]; then
            cluster_debug "Still waiting... (${elapsed}s/${timeout}s)"
        fi
    done

    cluster_error "Timeout waiting for $description after ${timeout}s"
    return 1
}

# Export functions for use in other scripts
export -f cluster_log cluster_warn cluster_error cluster_die cluster_debug
export -f cluster_load_config cluster_validate_config
export -f cluster_generate_id
export -f cluster_state_save cluster_state_load cluster_state_delete cluster_state_list
export -f cluster_state_get_file
export -f cluster_check_root cluster_check_deps
export -f cluster_wait_with_timeout
