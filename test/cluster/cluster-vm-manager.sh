#!/bin/bash
#
# cluster-vm-manager.sh - VM lifecycle management for LVM cluster testing
#
# Provides:
# - VM creation and destruction
# - IP address discovery
# - SSH setup
# - Package installation
# - Source deployment and compilation
# - Lock manager configuration
#

# Source the core library
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-test-lib.sh"

#
# VM naming and identification
#

cluster_vm_get_name() {
    local cluster_id="$1"
    local node_num="$2"

    echo "${cluster_id}-node${node_num}"
}

#
# VM creation
#

cluster_vm_create() {
    local node_num="$1"
    local cluster_id="$2"

    if [ -z "$node_num" ] || [ -z "$cluster_id" ]; then
        cluster_die "cluster_vm_create: node_num and cluster_id are required"
    fi

    local vm_name=$(cluster_vm_get_name "$cluster_id" "$node_num")

    cluster_log "Creating VM: $vm_name (node $node_num)"

    # Check if OS image is specified
    if [ -z "${CLUSTER_NODE_OS_IMAGE:-}" ]; then
        cluster_die "CLUSTER_NODE_OS_IMAGE is not set - please specify a cloud-ready OS image"
    fi

    if [ ! -f "$CLUSTER_NODE_OS_IMAGE" ]; then
        cluster_die "OS image not found: $CLUSTER_NODE_OS_IMAGE"
    fi

    # Create disk image directory
    local disk_dir="/var/lib/libvirt/images"
    local disk_image="${disk_dir}/${vm_name}.qcow2"

    # Create a copy-on-write disk from the base image
    cluster_log "Creating disk image: $disk_image"
    qemu-img create -f qcow2 -F qcow2 -b "$CLUSTER_NODE_OS_IMAGE" "$disk_image" "${CLUSTER_NODE_DISK_SIZE}G" || \
        cluster_die "Failed to create disk image: $disk_image"

    # Generate cloud-init configuration
    local cloudinit_dir="/var/tmp/cloudinit-${vm_name}"
    mkdir -p "$cloudinit_dir"

    # Create meta-data
    cat > "$cloudinit_dir/meta-data" <<EOF
instance-id: ${vm_name}
local-hostname: ${vm_name}
EOF

    # Create user-data with SSH key
    local ssh_key_file="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa.pub"
    if [ ! -f "$ssh_key_file" ]; then
        cluster_log "Generating SSH key for cluster testing"
        ssh-keygen -t rsa -b 4096 -f "${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa" -N "" -C "cluster-test" || \
            cluster_die "Failed to generate SSH key"
    fi

    local ssh_pubkey=$(cat "$ssh_key_file")

    cat > "$cloudinit_dir/user-data" <<EOF
#cloud-config
users:
  - name: ${CLUSTER_SSH_USER}
    ssh_authorized_keys:
      - ${ssh_pubkey}
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: false

# Allow root login
ssh_pwauth: false
disable_root: false

# Set root password (for console access if needed)
chpasswd:
  expire: false
  list: |
    root:cluster123

# Expand root filesystem
growpart:
  mode: auto
  devices: ['/']

runcmd:
  - systemctl enable sshd
  - systemctl start sshd
EOF

    # Create cloud-init ISO
    local cloudinit_iso="${disk_dir}/${vm_name}-cloudinit.iso"
    cluster_log "Creating cloud-init ISO: $cloudinit_iso"

    if command -v genisoimage &>/dev/null; then
        genisoimage -output "$cloudinit_iso" -volid cidata -joliet -rock \
            "$cloudinit_dir/user-data" "$cloudinit_dir/meta-data" || \
            cluster_die "Failed to create cloud-init ISO"
    elif command -v mkisofs &>/dev/null; then
        mkisofs -output "$cloudinit_iso" -volid cidata -joliet -rock \
            "$cloudinit_dir/user-data" "$cloudinit_dir/meta-data" || \
            cluster_die "Failed to create cloud-init ISO"
    else
        cluster_die "Neither genisoimage nor mkisofs found - required for cloud-init"
    fi

    # Clean up cloud-init directory
    rm -rf "$cloudinit_dir"

    # Create VM with virt-install
    cluster_log "Installing VM with virt-install"
    virt-install \
        --name "$vm_name" \
        --memory "$CLUSTER_NODE_MEMORY" \
        --vcpus "$CLUSTER_NODE_VCPUS" \
        --disk path="$disk_image",format=qcow2,bus=virtio \
        --disk path="$cloudinit_iso",device=cdrom \
        --network network="${CLUSTER_NETWORK_NAME}",model=virtio \
        --osinfo "${CLUSTER_NODE_OS_VARIANT:-linux2024}" \
        --graphics none \
        --noautoconsole \
        --import || cluster_die "Failed to create VM: $vm_name"

    cluster_log "VM created successfully: $vm_name"
}

#
# VM IP address discovery
#

cluster_vm_get_ip() {
    local vm_name="$1"

    if [ -z "$vm_name" ]; then
        cluster_die "cluster_vm_get_ip: vm_name is required"
    fi

    # Try virsh domifaddr first (works with DHCP)
    local ip=$(virsh domifaddr "$vm_name" 2>/dev/null | awk '/ipv4/ {print $4}' | cut -d'/' -f1 | head -n1)

    if [ -n "$ip" ]; then
        echo "$ip"
        return 0
    fi

    # Fallback: try getting IP from ARP table
    local mac=$(virsh domiflist "$vm_name" 2>/dev/null | awk '/network/ {print $5}' | head -n1)
    if [ -n "$mac" ]; then
        ip=$(arp -an | grep -i "$mac" | awk '{print $2}' | tr -d '()')
        if [ -n "$ip" ]; then
            echo "$ip"
            return 0
        fi
    fi

    return 1
}

#
# VM boot waiting
#

cluster_vm_wait_boot() {
    local vm_name="$1"
    local timeout="${CLUSTER_VM_BOOT_TIMEOUT:-300}"

    if [ -z "$vm_name" ]; then
        cluster_die "cluster_vm_wait_boot: vm_name is required"
    fi

    cluster_log "Waiting for VM to boot: $vm_name"

    # Wait for VM to be running
    cluster_wait_with_timeout "$timeout" 5 \
        "virsh domstate '$vm_name' 2>/dev/null | grep -q running" \
        "VM running state" || return 1

    # Wait for IP address
    cluster_log "Waiting for IP address assignment"
    local ip=""
    local elapsed=0

    while [ $elapsed -lt "$timeout" ]; do
        ip=$(cluster_vm_get_ip "$vm_name")
        if [ -n "$ip" ]; then
            cluster_log "VM IP address: $ip"
            echo "$ip"
            return 0
        fi

        sleep 5
        elapsed=$((elapsed + 5))
    done

    cluster_error "Failed to get IP address for VM: $vm_name"
    return 1
}

#
# SSH setup and connectivity
#

cluster_vm_setup_ssh() {
    local vm_ip="$1"
    local timeout="${CLUSTER_SSH_READY_TIMEOUT:-180}"

    if [ -z "$vm_ip" ]; then
        cluster_die "cluster_vm_setup_ssh: vm_ip is required"
    fi

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    local ssh_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR"

    cluster_log "Waiting for SSH to become available on $vm_ip"

    local elapsed=0
    while [ $elapsed -lt "$timeout" ]; do
        if ssh $ssh_opts -i "$ssh_key" "${CLUSTER_SSH_USER}@${vm_ip}" "true" 2>/dev/null; then
            cluster_log "SSH is ready on $vm_ip"
            return 0
        fi

        sleep 5
        elapsed=$((elapsed + 5))

        if [ $((elapsed % 30)) -eq 0 ]; then
            cluster_debug "Still waiting for SSH... (${elapsed}s/${timeout}s)"
        fi
    done

    cluster_error "SSH not available on $vm_ip after ${timeout}s"
    return 1
}

cluster_vm_ssh() {
    local vm_ip="$1"
    shift
    local cmd="$*"

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    local ssh_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

    ssh $ssh_opts -i "$ssh_key" "${CLUSTER_SSH_USER}@${vm_ip}" "$cmd"
}

cluster_vm_scp() {
    local src="$1"
    local dst="$2"

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    local scp_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

    scp $scp_opts -i "$ssh_key" -r "$src" "$dst"
}

#
# Package installation
#

cluster_vm_install_packages() {
    local vm_ip="$1"
    local node_num="$2"

    if [ -z "$vm_ip" ] || [ -z "$node_num" ]; then
        cluster_die "cluster_vm_install_packages: vm_ip and node_num are required"
    fi

    cluster_log "Installing packages on node $node_num ($vm_ip)"

    # Detect package manager
    local pkg_mgr=""
    if cluster_vm_ssh "$vm_ip" "command -v dnf" &>/dev/null; then
        pkg_mgr="dnf"
    elif cluster_vm_ssh "$vm_ip" "command -v yum" &>/dev/null; then
        pkg_mgr="yum"
    elif cluster_vm_ssh "$vm_ip" "command -v apt-get" &>/dev/null; then
        pkg_mgr="apt-get"
    else
        cluster_die "No supported package manager found on node $node_num"
    fi

    cluster_debug "Detected package manager: $pkg_mgr"

    local packages=()

    if [ "$node_num" = "0" ]; then
        # Node 0: Storage exporter packages
        cluster_log "Installing storage exporter packages on node 0"
        if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
            packages+=(targetcli python3-rtslib nvme-cli sg3_utils)
        else
            packages+=(targetcli-fb nvme-cli sg3-utils)
        fi
    else
        # Test nodes (1..N): Install based on configuration

        # Always install initiator tools
        if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
            packages+=(iscsi-initiator-utils nvme-cli sg3_utils)
            # Add multipath if enabled
            if [ "${CLUSTER_MULTIPATH_ENABLE:-0}" = "1" ]; then
                packages+=(device-mapper-multipath)
            fi
        else
            packages+=(open-iscsi nvme-cli sg3-utils)
            # Add multipath if enabled
            if [ "${CLUSTER_MULTIPATH_ENABLE:-0}" = "1" ]; then
                packages+=(multipath-tools)
            fi
        fi

        # LVM packages or build dependencies
        if [ "${CLUSTER_USE_SOURCE:-0}" = "1" ]; then
            cluster_log "Installing LVM build dependencies on node $node_num"
            if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
                packages+=(gcc make autoconf automake libtool pkgconfig)
                packages+=(libaio-devel libudev-devel libblkid-devel device-mapper-devel)
                packages+=(readline-devel ncurses-devel systemd-devel)
                packages+=(libnvme-devel xfsprogs-devel)
            else
                packages+=(build-essential autoconf automake libtool pkg-config)
                packages+=(libaio-dev libudev-dev libblkid-dev libdevmapper-dev)
                packages+=(libreadline-dev libncurses-dev libsystemd-dev)
                packages+=(libnvme-dev xfsprogs-dev)
            fi
        else
            cluster_log "Installing LVM packages on node $node_num"
            if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
                packages+=(lvm2 lvm2-lockd)
            else
                packages+=(lvm2)
            fi
        fi

        # Lock manager packages or build dependencies
        if [ "$CLUSTER_LOCK_TYPE" = "sanlock" ]; then
            if [ "${SANLOCK_USE_SOURCE:-0}" = "1" ]; then
                cluster_log "Installing sanlock build dependencies on node $node_num"
                if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
                    packages+=(libaio-devel libblkid-devel systemd-devel libuuid-devel device-mapper-devel)
                else
                    packages+=(libaio-dev libblkid-dev libsystemd-dev uuid-dev libdevmapper-dev)
                fi
            else
                cluster_log "Installing sanlock packages on node $node_num"
                if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
                    packages+=(sanlock sanlock-lib wdmd)
                else
                    packages+=(sanlock)
                fi
            fi
        elif [ "$CLUSTER_LOCK_TYPE" = "dlm" ]; then
            cluster_log "Installing DLM packages on node $node_num"
            if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
                packages+=(corosync dlm dlm-lib dlm-devel)
                # Install kernel-modules-extra matching the running kernel
                # This will be done separately after package install to ensure version match
            else
                packages+=(corosync)
            fi
        fi

        # Add base packages
        if [ -n "${CLUSTER_BASE_PACKAGES:-}" ]; then
            read -ra base_pkgs <<< "$CLUSTER_BASE_PACKAGES"
            packages+=("${base_pkgs[@]}")
        fi
    fi

    # Install packages
    if [ ${#packages[@]} -gt 0 ]; then
        cluster_log "Installing: ${packages[*]}"

        if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
            cluster_vm_ssh "$vm_ip" "$pkg_mgr install -y ${packages[*]}" || \
                cluster_die "Package installation failed on node $node_num"
        else
            cluster_vm_ssh "$vm_ip" "apt-get update && apt-get install -y ${packages[*]}" || \
                cluster_die "Package installation failed on node $node_num"
        fi

        cluster_log "Packages installed successfully on node $node_num"
    fi

    # Install kernel-modules-extra for DLM (must match running kernel version)
    if [ "$node_num" != "0" ] && [ "$CLUSTER_LOCK_TYPE" = "dlm" ]; then
        cluster_log "Installing kernel-modules-extra for DLM on node $node_num"
        if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
            cluster_vm_ssh "$vm_ip" "$pkg_mgr install -y \"kernel-modules-extra-\$(uname -r)\"" || \
                cluster_warn "Failed to install kernel-modules-extra for current kernel"
        else
            cluster_vm_ssh "$vm_ip" "apt-get install -y linux-modules-extra-\$(uname -r)" || \
                cluster_warn "Failed to install linux-modules-extra for current kernel"
        fi
    fi

    # Load extra packages from file if specified
    if [ -n "${CLUSTER_EXTRA_PACKAGES_FILE:-}" ] && [ -f "$CLUSTER_EXTRA_PACKAGES_FILE" ]; then
        cluster_log "Installing extra packages from: $CLUSTER_EXTRA_PACKAGES_FILE"
        local extra_packages=$(cat "$CLUSTER_EXTRA_PACKAGES_FILE" | tr '\n' ' ')

        if [[ "$pkg_mgr" =~ ^(dnf|yum)$ ]]; then
            cluster_vm_ssh "$vm_ip" "$pkg_mgr install -y $extra_packages" || \
                cluster_warn "Some extra packages failed to install"
        else
            cluster_vm_ssh "$vm_ip" "apt-get install -y $extra_packages" || \
                cluster_warn "Some extra packages failed to install"
        fi
    fi
}

#
# Source deployment
#

cluster_vm_deploy_sanlock_source() {
    local vm_ip="$1"
    local node_num="$2"

    if [ "$node_num" = "0" ]; then
        cluster_debug "Skipping sanlock source deployment on node 0 (storage exporter)"
        return 0
    fi

    if [ "${SANLOCK_USE_SOURCE:-0}" != "1" ]; then
        return 0
    fi

    if [ "$CLUSTER_LOCK_TYPE" != "sanlock" ]; then
        return 0
    fi

    cluster_log "Deploying sanlock source to node $node_num ($vm_ip)"

    if [ -z "${SANLOCK_SOURCE_DIR:-}" ]; then
        cluster_die "SANLOCK_SOURCE_DIR is not set"
    fi

    if [ ! -d "$SANLOCK_SOURCE_DIR" ]; then
        cluster_die "Sanlock source directory not found: $SANLOCK_SOURCE_DIR"
    fi

    local remote_dir="/root/sanlock-source"

    # Create remote directory
    cluster_vm_ssh "$vm_ip" "mkdir -p $remote_dir"

    # Copy source tree (exclude .git and build artifacts)
    cluster_log "Copying sanlock source tree to $vm_ip:$remote_dir"
    rsync -az --exclude='.git' --exclude='*.o' --exclude='*.a' \
        -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -i ${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa" \
        "$SANLOCK_SOURCE_DIR/" "${CLUSTER_SSH_USER}@${vm_ip}:${remote_dir}/" || \
        cluster_die "Failed to copy sanlock source to node $node_num"

    # Build sanlock
    cluster_log "Building sanlock on node $node_num"
    cluster_vm_ssh "$vm_ip" "cd $remote_dir && make LIBDIR=/usr/lib64 ${SANLOCK_BUILD_OPTS:-}" || \
        cluster_die "Failed to build sanlock on node $node_num"

    # Install sanlock
    cluster_log "Installing sanlock on node $node_num"
    cluster_vm_ssh "$vm_ip" "cd $remote_dir && make install LIBDIR=/usr/lib64" || \
        cluster_die "Failed to install sanlock on node $node_num"

    # Install systemd-wdmd helper
    cluster_log "Installing systemd-wdmd helper on node $node_num"
    cluster_vm_ssh "$vm_ip" "
        cd $remote_dir
        # systemd-wdmd is in init.d/ subdirectory
        if [ -f init.d/systemd-wdmd ]; then
            install -D -m 755 init.d/systemd-wdmd /lib/systemd/systemd-wdmd
        elif [ -f wdmd/systemd-wdmd ]; then
            install -D -m 755 wdmd/systemd-wdmd /lib/systemd/systemd-wdmd
        elif [ -f systemd-wdmd ]; then
            install -D -m 755 systemd-wdmd /lib/systemd/systemd-wdmd
        else
            echo 'WARNING: systemd-wdmd helper not found in source tree'
        fi
    "

    # Run ldconfig to update library cache
    cluster_vm_ssh "$vm_ip" "ldconfig"

    # Install systemd service files from source tree if they exist
    cluster_log "Installing systemd service files for sanlock on node $node_num"
    cluster_vm_ssh "$vm_ip" "
        cd $remote_dir
        # Try various locations where service files might be
        for svc_file in init.d/wdmd.service src/wdmd.service wdmd/wdmd.service; do
            if [ -f \"\$svc_file\" ]; then
                cp \"\$svc_file\" /etc/systemd/system/wdmd.service
                break
            fi
        done
        for svc_file in init.d/sanlock.service.native init.d/sanlock.service src/sanlock.service; do
            if [ -f \"\$svc_file\" ]; then
                cp \"\$svc_file\" /etc/systemd/system/sanlock.service
                break
            fi
        done
        systemctl daemon-reload
    "

    # Verify installation
    cluster_log "Verifying sanlock installation on node $node_num"
    cluster_vm_ssh "$vm_ip" "sanlock version" || \
        cluster_die "Sanlock installation verification failed on node $node_num"

    cluster_log "Sanlock source deployed successfully on node $node_num"
}

cluster_vm_deploy_lvm_source() {
    local vm_ip="$1"
    local node_num="$2"

    if [ "$node_num" = "0" ]; then
        cluster_debug "Skipping LVM source deployment on node 0 (storage exporter)"
        return 0
    fi

    if [ "${CLUSTER_USE_SOURCE:-0}" != "1" ]; then
        return 0
    fi

    cluster_log "Deploying LVM source to node $node_num ($vm_ip)"

    if [ -z "${CLUSTER_SOURCE_DIR:-}" ]; then
        cluster_die "CLUSTER_SOURCE_DIR is not set"
    fi

    if [ ! -d "$CLUSTER_SOURCE_DIR" ]; then
        cluster_die "LVM source directory not found: $CLUSTER_SOURCE_DIR"
    fi

    local remote_dir="/root/lvm-source"

    # Create remote directory
    cluster_vm_ssh "$vm_ip" "mkdir -p $remote_dir"

    # Copy source tree (exclude .git and build artifacts)
    cluster_log "Copying LVM source tree to $vm_ip:$remote_dir"
    rsync -az --exclude='.git' --exclude='*.o' --exclude='*.a' --exclude='test/cluster' \
        -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -i ${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa" \
        "$CLUSTER_SOURCE_DIR/" "${CLUSTER_SSH_USER}@${vm_ip}:${remote_dir}/" || \
        cluster_die "Failed to copy LVM source to node $node_num"

    # Configure and build
    cluster_log "Configuring LVM on node $node_num"

    # If sanlock was built from source, set PKG_CONFIG_PATH
    local configure_env=""
    if [ "${SANLOCK_USE_SOURCE:-0}" = "1" ] && [ "$CLUSTER_LOCK_TYPE" = "sanlock" ]; then
        configure_env="PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/lib64/pkgconfig:$PKG_CONFIG_PATH"
    fi

    # Build configure options to match production build
    local configure_opts="${CLUSTER_BUILD_OPTS:-}"

    # Base paths (use standard RPM paths)
    configure_opts="$configure_opts --prefix=/usr"
    configure_opts="$configure_opts --sbindir=/usr/sbin"
    configure_opts="$configure_opts --libdir=/usr/lib64"
    configure_opts="$configure_opts --with-usrlibdir=/usr/lib64"

    # Runtime directories (match production paths under /run)
    configure_opts="$configure_opts --with-default-dm-run-dir=/run"
    configure_opts="$configure_opts --with-default-run-dir=/run/lvm"
    configure_opts="$configure_opts --with-default-pid-dir=/run"
    configure_opts="$configure_opts --with-default-locking-dir=/run/lock/lvm"

    # Lock manager (specific to configured type)
    if [ "$CLUSTER_LOCK_TYPE" = "sanlock" ]; then
        configure_opts="$configure_opts --enable-lvmlockd-sanlock"
    elif [ "$CLUSTER_LOCK_TYPE" = "dlm" ]; then
        configure_opts="$configure_opts --enable-lvmlockd-dlm"
    fi

    # Core features (match production build)
    configure_opts="$configure_opts --enable-debug"
    configure_opts="$configure_opts --enable-lvmpolld"
    configure_opts="$configure_opts --enable-dmeventd"
    configure_opts="$configure_opts --enable-udev_sync"
    configure_opts="$configure_opts --with-thin=internal"
    configure_opts="$configure_opts --enable-blkid_wiping"
    configure_opts="$configure_opts --with-default-use-devices-file=1"

    # Detect systemd unit directory (use system default, typically /usr/lib/systemd/system)
    local systemd_unit_dir
    systemd_unit_dir=$(cluster_vm_ssh "$vm_ip" "pkg-config --variable=systemdsystemunitdir systemd 2>/dev/null" || echo "/usr/lib/systemd/system")
    configure_opts="$configure_opts --with-systemdsystemunitdir=$systemd_unit_dir"

    cluster_vm_ssh "$vm_ip" "cd $remote_dir && $configure_env ./configure $configure_opts" || \
        cluster_die "Failed to configure LVM on node $node_num"

    cluster_log "Building LVM on node $node_num"
    cluster_vm_ssh "$vm_ip" "cd $remote_dir && make -j\$(nproc)" || \
        cluster_die "Failed to build LVM on node $node_num"

    # Install
    cluster_log "Installing LVM on node $node_num"
    cluster_vm_ssh "$vm_ip" "cd $remote_dir && make install" || \
        cluster_die "Failed to install LVM on node $node_num"

    # Run ldconfig to update library cache
    cluster_vm_ssh "$vm_ip" "ldconfig"

    # Install systemd service files if they exist but weren't installed by make install
    cluster_log "Checking systemd service files for lvmlockd on node $node_num"
    cluster_vm_ssh "$vm_ip" "
        cd $remote_dir
        # If lvmlockd.service doesn't exist in systemd, try to install it
        if [ ! -f $systemd_unit_dir/lvmlockd.service ]; then
            # Check if configure created the service file in the build tree
            if [ -f scripts/lvmlockd.service ]; then
                cp scripts/lvmlockd.service $systemd_unit_dir/
            fi
        fi
        systemctl daemon-reload
    "

    # Verify installation
    cluster_log "Verifying LVM installation on node $node_num"
    cluster_vm_ssh "$vm_ip" "lvm version" || \
        cluster_die "LVM installation verification failed on node $node_num"

    cluster_log "LVM source deployed successfully on node $node_num"
}

#
# Lock manager setup
#

cluster_vm_setup_lock_manager() {
    local vm_ip="$1"
    local node_num="$2"

    if [ "$node_num" = "0" ]; then
        cluster_debug "Skipping lock manager setup on node 0 (storage exporter)"
        return 0
    fi

    cluster_log "Setting up lock manager on node $node_num ($vm_ip)"

    if [ "$CLUSTER_LOCK_TYPE" = "sanlock" ]; then
        # Configure sanlock with unique host_id per node
        cluster_log "Configuring sanlock with host_id=$node_num on node $node_num"

        # Create lvmlocal.conf with host_id
        cluster_vm_ssh "$vm_ip" "mkdir -p /etc/lvm"
        cluster_vm_ssh "$vm_ip" "cat > /etc/lvm/lvmlocal.conf <<'EOF'
local {
    host_id = $node_num
}
EOF"

        # Enable and start services
        cluster_log "Starting sanlock on node $node_num"
        cluster_vm_ssh "$vm_ip" "systemctl daemon-reload"
        cluster_vm_ssh "$vm_ip" "systemctl enable wdmd sanlock"
        cluster_vm_ssh "$vm_ip" "systemctl start wdmd"
        cluster_vm_ssh "$vm_ip" "systemctl start sanlock"

        # Verify sanlock is running
        if ! cluster_vm_ssh "$vm_ip" "systemctl is-active sanlock" &>/dev/null; then
            # Show status for debugging
            cluster_vm_ssh "$vm_ip" "systemctl status sanlock" || true
            cluster_die "Sanlock failed to start on node $node_num"
        fi

        cluster_log "Sanlock configured and running on node $node_num"

    elif [ "$CLUSTER_LOCK_TYPE" = "dlm" ]; then
        cluster_log "Configuring DLM/Corosync on node $node_num"

        # Generate corosync.conf with all test nodes
        local corosync_conf="/tmp/corosync.conf.$$"

        cat > "$corosync_conf" <<EOF
totem {
        version: 2
        cluster_name: ${CLUSTER_ID}
        crypto_cipher: none
        crypto_hash: none
}

logging {
        fileline: off
        to_stderr: yes
        to_logfile: yes
        logfile: /var/log/cluster/corosync.log
        to_syslog: yes
        debug: off
        logger_subsys {
                subsys: QUORUM
                debug: off
        }
}

quorum {
        provider: corosync_votequorum
}

nodelist {
EOF

        # Add all test nodes to nodelist
        for i in $(seq 1 "$CLUSTER_NUM_NODES"); do
            local node_ip="${CLUSTER_NODE_IPS[$i]}"
            local node_name="${CLUSTER_ID}-node${i}"

            cluster_debug "Adding node $i to corosync.conf: name=${node_name}, nodeid=${i}, ring0_addr=${node_ip}"

            if [ -z "$node_ip" ]; then
                cluster_die "Node IP is empty for node $i - CLUSTER_NODE_IPS array may not be set correctly"
            fi

            cat >> "$corosync_conf" <<EOF
        node {
                name: ${node_name}
                nodeid: ${i}
                ring0_addr: ${node_ip}
        }
EOF
        done

        cat >> "$corosync_conf" <<EOF
}
EOF

        # Debug: show generated config
        cluster_debug "Generated corosync.conf:"
        cluster_debug "$(cat "$corosync_conf")"

        # Deploy corosync.conf to this node
        cluster_vm_ssh "$vm_ip" "mkdir -p /etc/corosync /var/log/cluster"
        cluster_vm_scp "$corosync_conf" "${CLUSTER_SSH_USER}@${vm_ip}:/etc/corosync/corosync.conf"

        # Verify deployed config
        cluster_log "Verifying corosync.conf on node $node_num"
        if ! cluster_vm_ssh "$vm_ip" "grep -q 'ring0_addr:' /etc/corosync/corosync.conf"; then
            cluster_error "corosync.conf is missing ring0_addr entries!"
            cluster_vm_ssh "$vm_ip" "cat /etc/corosync/corosync.conf" || true
            cluster_die "Invalid corosync.conf generated"
        fi

        rm -f "$corosync_conf"

        # Enable and start corosync
        cluster_log "Starting corosync on node $node_num"
        cluster_vm_ssh "$vm_ip" "systemctl enable corosync"
        cluster_vm_ssh "$vm_ip" "systemctl start corosync"

        # Verify corosync is running
        if ! cluster_vm_ssh "$vm_ip" "systemctl is-active corosync" &>/dev/null; then
            cluster_vm_ssh "$vm_ip" "systemctl status corosync" || true
            cluster_die "Corosync failed to start on node $node_num"
        fi

        # Load DLM kernel module
        cluster_log "Loading DLM kernel module on node $node_num"
        cluster_vm_ssh "$vm_ip" "mkdir -p /etc/modules-load.d && echo 'dlm' > /etc/modules-load.d/dlm.conf"

        # Verify module exists before trying to load
        if ! cluster_vm_ssh "$vm_ip" "ls /lib/modules/\$(uname -r)/kernel/fs/dlm/dlm.ko* 2>/dev/null" &>/dev/null; then
            cluster_error "DLM kernel module not found"
            cluster_error "Kernel version: $(cluster_vm_ssh "$vm_ip" "uname -r")"
            cluster_error "Installed kernel packages:"
            cluster_vm_ssh "$vm_ip" "rpm -qa | grep kernel" || true
            cluster_die "DLM kernel module not available - kernel-modules-extra may not be installed or kernel mismatch"
        fi

        if ! cluster_vm_ssh "$vm_ip" "modprobe dlm"; then
            cluster_error "Failed to load dlm kernel module"
            cluster_vm_ssh "$vm_ip" "dmesg | tail -20" || true
            cluster_die "DLM kernel module failed to load on node $node_num"
        fi

        # Enable and start DLM
        cluster_log "Starting DLM on node $node_num"
        cluster_vm_ssh "$vm_ip" "systemctl enable dlm"
        cluster_vm_ssh "$vm_ip" "systemctl start dlm"

        # Verify DLM is running
        if ! cluster_vm_ssh "$vm_ip" "systemctl is-active dlm" &>/dev/null; then
            cluster_vm_ssh "$vm_ip" "systemctl status dlm" || true
            cluster_die "DLM failed to start on node $node_num"
        fi

        cluster_log "DLM/Corosync configured and running on node $node_num"
    fi

    # Configure LVM to use lvmlockd (for sanlock and dlm)
    if [ "$CLUSTER_LOCK_TYPE" = "sanlock" ] || [ "$CLUSTER_LOCK_TYPE" = "dlm" ]; then
        cluster_log "Configuring LVM to use lvmlockd on node $node_num"
        cluster_vm_ssh "$vm_ip" "mkdir -p /etc/lvm"

        # Create minimal lvm.conf with use_lvmlockd=1
        # Back up existing config if present
        cluster_vm_ssh "$vm_ip" "
            if [ -f /etc/lvm/lvm.conf ]; then
                cp /etc/lvm/lvm.conf /etc/lvm/lvm.conf.backup
            fi
            cat > /etc/lvm/lvm.conf <<'LVMCONF'
global {
    use_lvmlockd = 1
}
LVMCONF
        "

        cluster_debug "Configured use_lvmlockd=1 in /etc/lvm/lvm.conf on node $node_num"
    fi

    # Start lvmlockd
    cluster_log "Starting lvmlockd on node $node_num"
    cluster_vm_ssh "$vm_ip" "systemctl daemon-reload"
    cluster_vm_ssh "$vm_ip" "systemctl enable lvmlockd"
    cluster_vm_ssh "$vm_ip" "systemctl start lvmlockd"

    # Verify lvmlockd is running
    if ! cluster_vm_ssh "$vm_ip" "systemctl is-active lvmlockd" &>/dev/null; then
        # Show status for debugging
        cluster_vm_ssh "$vm_ip" "systemctl status lvmlockd" || true
        cluster_die "lvmlockd failed to start on node $node_num"
    fi

    cluster_log "Lock manager setup complete on node $node_num"
}

#
# VM destruction
#

cluster_vm_destroy() {
    local vm_name="$1"

    if [ -z "$vm_name" ]; then
        cluster_die "cluster_vm_destroy: vm_name is required"
    fi

    cluster_log "Destroying VM: $vm_name"

    # Stop VM if running
    if virsh domstate "$vm_name" 2>/dev/null | grep -q running; then
        cluster_log "Stopping VM: $vm_name"
        virsh destroy "$vm_name" 2>/dev/null || true
    fi

    # Undefine VM
    if virsh dominfo "$vm_name" &>/dev/null; then
        cluster_log "Undefining VM: $vm_name"
        virsh undefine "$vm_name" --remove-all-storage 2>/dev/null || true
    fi

    # Clean up cloud-init ISO
    local disk_dir="/var/lib/libvirt/images"
    local cloudinit_iso="${disk_dir}/${vm_name}-cloudinit.iso"
    if [ -f "$cloudinit_iso" ]; then
        rm -f "$cloudinit_iso"
    fi

    cluster_log "VM destroyed: $vm_name"
}

#
# Storage export setup
#

cluster_vm_setup_storage_export() {
    local node0_ip="$1"
    local cluster_id="$2"

    if [ -z "$node0_ip" ] || [ -z "$cluster_id" ]; then
        cluster_die "cluster_vm_setup_storage_export: node0_ip and cluster_id are required"
    fi

    cluster_log "Setting up storage export on node 0 ($node0_ip)"

    # Copy required scripts to node 0
    local remote_dir="/root/cluster-scripts"
    cluster_vm_ssh "$node0_ip" "mkdir -p $remote_dir"

    cluster_log "Copying cluster scripts to node 0"
    cluster_vm_scp "$SCRIPT_DIR/cluster-test-lib.sh" "${CLUSTER_SSH_USER}@${node0_ip}:${remote_dir}/"
    cluster_vm_scp "$SCRIPT_DIR/cluster-storage-exporter.sh" "${CLUSTER_SSH_USER}@${node0_ip}:${remote_dir}/"

    # Make scripts executable
    cluster_vm_ssh "$node0_ip" "chmod +x ${remote_dir}/*.sh"

    # Execute storage setup on node 0 with required environment variables
    cluster_log "Executing storage export setup on node 0"
    cluster_vm_ssh "$node0_ip" "
        export CLUSTER_STORAGE_TYPE='${CLUSTER_STORAGE_TYPE}'
        export CLUSTER_NUM_DEVICES='${CLUSTER_NUM_DEVICES}'
        export CLUSTER_DEVICE_SIZE='${CLUSTER_DEVICE_SIZE}'
        export CLUSTER_DEVICE_SECTOR_SIZE='${CLUSTER_DEVICE_SECTOR_SIZE:-512}'
        export CLUSTER_BACKING_TYPE='${CLUSTER_BACKING_TYPE}'
        export NODE0_IP='${node0_ip}'
        export CLUSTER_DEBUG='${CLUSTER_DEBUG:-0}'

        cd ${remote_dir}
        ./cluster-storage-exporter.sh '${cluster_id}'
    " || {
        cluster_error "Failed to setup storage export on node 0"
        return 1
    }

    cluster_log "Storage export setup complete on node 0"
    cluster_log "  Storage type: ${CLUSTER_STORAGE_TYPE}"
    cluster_log "  Backing type: ${CLUSTER_BACKING_TYPE}"
    cluster_log "  Number of devices: ${CLUSTER_NUM_DEVICES}"
    cluster_log "  Device size: ${CLUSTER_DEVICE_SIZE}MB"

    return 0
}

#
# Storage import setup
#

cluster_vm_setup_storage_import() {
    local node_ip="$1"
    local node_num="$2"
    local node0_ip="$3"
    local cluster_id="$4"

    if [ -z "$node_ip" ] || [ -z "$node_num" ] || [ -z "$node0_ip" ] || [ -z "$cluster_id" ]; then
        cluster_die "cluster_vm_setup_storage_import: all arguments are required"
    fi

    cluster_log "Setting up storage import on node $node_num ($node_ip)"

    # Copy storage importer script to test node
    local remote_dir="/root/cluster-scripts"
    cluster_vm_ssh "$node_ip" "mkdir -p $remote_dir"

    cluster_debug "Copying cluster-storage-importer.sh to node $node_num"
    cluster_vm_scp "$SCRIPT_DIR/cluster-storage-importer.sh" "${CLUSTER_SSH_USER}@${node_ip}:${remote_dir}/"

    # Copy cluster-test-lib.sh (needed by storage importer)
    cluster_debug "Copying cluster-test-lib.sh to node $node_num"
    cluster_vm_scp "$SCRIPT_DIR/cluster-test-lib.sh" "${CLUSTER_SSH_USER}@${node_ip}:${remote_dir}/"

    # Make scripts executable
    cluster_vm_ssh "$node_ip" "chmod +x ${remote_dir}/*.sh"

    # Execute storage import on test node with required environment variables
    cluster_log "Executing storage import on node $node_num"
    cluster_vm_ssh "$node_ip" "
        export CLUSTER_STORAGE_TYPE='${CLUSTER_STORAGE_TYPE}'
        export CLUSTER_MULTIPATH_ENABLE='${CLUSTER_MULTIPATH_ENABLE:-0}'
        export NODE0_IP='${node0_ip}'
        export THIS_NODE_IP='${node_ip}'
        export CLUSTER_DEBUG='${CLUSTER_DEBUG:-0}'

        cd ${remote_dir}
        ./cluster-storage-importer.sh '${cluster_id}' '${node_num}'
    " || {
        cluster_error "Failed to import storage on node $node_num"
        return 1
    }

    cluster_log "Storage import complete on node $node_num"
    return 0
}

#
# Batch operations
#

cluster_vms_create_all() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_die "cluster_vms_create_all: cluster_id is required"
    fi

    local num_nodes="${CLUSTER_NUM_NODES:-3}"
    local node_ips=()

    # Create node 0 (storage exporter)
    cluster_log "Creating node 0 (storage exporter)"
    cluster_vm_create 0 "$cluster_id"

    local node0_ip=$(cluster_vm_wait_boot "$(cluster_vm_get_name "$cluster_id" 0)")
    if [ -z "$node0_ip" ]; then
        cluster_die "Failed to get IP for node 0"
    fi

    cluster_vm_setup_ssh "$node0_ip"
    cluster_vm_install_packages "$node0_ip" 0

    # Set up storage export on node 0
    cluster_vm_setup_storage_export "$node0_ip" "$cluster_id"

    node_ips+=("$node0_ip")

    # Create test nodes 1..N and build IP array
    for node_num in $(seq 1 "$num_nodes"); do
        cluster_log "Creating node $node_num (test node)"
        cluster_vm_create "$node_num" "$cluster_id"

        local node_ip=$(cluster_vm_wait_boot "$(cluster_vm_get_name "$cluster_id" "$node_num")")
        if [ -z "$node_ip" ]; then
            cluster_die "Failed to get IP for node $node_num"
        fi

        cluster_vm_setup_ssh "$node_ip"
        cluster_vm_install_packages "$node_ip" "$node_num"

        # Deploy sanlock source first (if enabled)
        cluster_vm_deploy_sanlock_source "$node_ip" "$node_num"

        # Deploy LVM source (if enabled)
        cluster_vm_deploy_lvm_source "$node_ip" "$node_num"

        node_ips+=("$node_ip")
    done

    # Export node IPs for later use (needed for corosync config generation)
    export CLUSTER_NODE_IPS=("${node_ips[@]}")

    cluster_log "All VMs created, node IPs: ${CLUSTER_NODE_IPS[*]}"

    # Import storage on test nodes from node 0
    cluster_log "Setting up storage import on test nodes"
    for node_num in $(seq 1 "$num_nodes"); do
        local node_ip="${CLUSTER_NODE_IPS[$node_num]}"
        cluster_vm_setup_storage_import "$node_ip" "$node_num" "$node0_ip" "$cluster_id" || {
            cluster_error "Failed to setup storage import on node $node_num"
            return 1
        }
    done

    # Now set up lock managers (requires all node IPs for DLM/corosync)
    cluster_log "Setting up lock managers on test nodes"
    for node_num in $(seq 1 "$num_nodes"); do
        local node_ip="${CLUSTER_NODE_IPS[$node_num]}"
        cluster_vm_setup_lock_manager "$node_ip" "$node_num"
    done

    cluster_log "All VMs created successfully"
    cluster_log "Node IPs: ${CLUSTER_NODE_IPS[*]}"
}

cluster_vms_destroy_all() {
    local cluster_id="$1"

    if [ -z "$cluster_id" ]; then
        cluster_die "cluster_vms_destroy_all: cluster_id is required"
    fi

    local num_nodes="${CLUSTER_NUM_NODES:-3}"

    # Clean up storage on node 0 before destroying VMs
    local node0_vm_name=$(cluster_vm_get_name "$cluster_id" 0)
    if virsh dominfo "$node0_vm_name" &>/dev/null; then
        # Get node 0 IP
        local node0_ip=$(cluster_vm_get_ip "$node0_vm_name" 2>/dev/null || true)
        if [ -n "$node0_ip" ]; then
            cluster_log "Cleaning up storage on node 0 ($node0_ip)"
            # Execute storage cleanup on node 0
            cluster_vm_ssh "$node0_ip" "
                export CLUSTER_DEBUG='${CLUSTER_DEBUG:-0}'
                cd /root/cluster-scripts 2>/dev/null || cd /tmp
                if [ -f cluster-storage-exporter.sh ]; then
                    source ./cluster-test-lib.sh
                    source ./cluster-storage-exporter.sh
                    cluster_storage_cleanup '${cluster_id}' '${CLUSTER_STORAGE_TYPE:-both}'
                fi
            " 2>/dev/null || cluster_warn "Failed to cleanup storage on node 0 (VM may be down)"
        fi
    fi

    # Destroy all nodes (0 through N)
    for node_num in $(seq 0 "$num_nodes"); do
        local vm_name=$(cluster_vm_get_name "$cluster_id" "$node_num")
        if virsh dominfo "$vm_name" &>/dev/null; then
            cluster_vm_destroy "$vm_name"
        fi
    done

    cluster_log "All VMs destroyed"
}

# Export functions
export -f cluster_vm_get_name
export -f cluster_vm_create cluster_vm_destroy
export -f cluster_vm_get_ip cluster_vm_wait_boot
export -f cluster_vm_setup_ssh cluster_vm_ssh cluster_vm_scp
export -f cluster_vm_install_packages
export -f cluster_vm_deploy_lvm_source cluster_vm_deploy_sanlock_source
export -f cluster_vm_setup_lock_manager
export -f cluster_vm_setup_storage_export cluster_vm_setup_storage_import
export -f cluster_vms_create_all cluster_vms_destroy_all
