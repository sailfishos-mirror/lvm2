#!/usr/bin/env bash
# Copyright (C) 2011-2025 Red Hat, Inc.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#
# LVM Command Wrapper for Test Suite
# ===================================
#
# This wrapper provides efficient LVM command execution for the test suite by:
# - Avoiding bash fork overhead through shell function wrapping
# - Supporting debug tools (GDB, Valgrind, strace)
# - Working in both build-time and installed test environments
# - Dynamically generating wrapper functions for LVM commands
#
# Usage:
#   Source this script to get LVM command wrappers (lvcreate, pvs, vgs, etc.)
#   Run directly to execute a single LVM command via wrapper
#

#
# __lvm_command_wrapper - Core wrapper function for LVM command execution
#
# This function is called by all generated LVM command wrappers (lvcreate, pvs, etc.)
# It determines which LVM command to execute and handles debug environments.
#
# Command name resolution:
#   - When sourced: Uses FUNCNAME[1] to get calling function name
#   - When executed directly: Uses script basename
#   - Special cases where command is passed as $1:
#     * lvm() function: enables "lvm <command>" syntax
#     * Internal use: FUNCNAME[1]="source" during wrapper generation
#
# Debug environment support:
#   - LVM_GDB: Run command under GDB debugger
#   - LVM_VALGRIND: Numeric value, non-zero enables Valgrind (filtered by LVM_DEBUG_LEVEL)
#   - LVM_STRACE: Run command under strace with specified options
#   - LVM_DEBUG_LEVEL: Controls which commands get traced (0=none, 1=modifiers, 2=all)
#
# Note: Trace disable/restore for 'set -x' is handled in generated wrapper functions
#
__lvm_command_wrapper() {
	# Initialize variables with defaults for sourced mode
	local CMD=${FUNCNAME[1]} # Calling function name (e.g., lvcreate, pvs)
	local EXEC=		# Empty - no exec needed when sourced
	local CALL=()
	local RUN_DBG=

	# Override defaults if script is executed directly (not sourced)
	if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
		CMD=${0##*/}	# Use script basename as command name
		EXEC=exec	# Use exec to replace shell process
	fi

	# Special handling: extract actual command from $1
	# - Called via lvm() function: enables "lvm <command>" syntax
	# - Called during sourcing: FUNCNAME[1]="source" for command discovery
	if [[ "$CMD" = "lvm" || "$CMD" = "source" ]]; then
		CMD=$1
		shift
	fi

	#
	# Debug mode: GDB
	#
	# When LVM_GDB is set, run command under GDB debugger.
	# Usage from test:
	#   LVM_GDB=1 sh shell/test-name.sh
	# Then in gdb prompt: run
	#
	if [[ -n "${LVM_GDB-}" ]]; then
		$EXEC gdb --readnow --args "$abs_top_builddir/tools/lvm" "$CMD" "$@"
		return
	fi

	#
	# Debug mode: Valgrind/strace setup
	#
	# Configure debug tool based on environment variables.
	# Debug level filtering prevents excessive tracing of read-only commands.
	#
	# Valgrind: Enabled when LVM_VALGRIND is non-zero
	# Uses VALGRIND variable if set, otherwise defaults to 'valgrind'
	[[ "${LVM_VALGRIND:-0}" -ne 0 ]] && RUN_DBG="${VALGRIND:-valgrind}"

	# Strace: Enabled when LVM_STRACE is set (overrides Valgrind if both set)
	[[ -n "${LVM_STRACE-}" ]] && RUN_DBG="strace $LVM_STRACE -o strace.log"
	# Filter debug tracing based on LVM_DEBUG_LEVEL
	# Level 0 (default): No tracing
	# Level 1: Trace modifying commands only (pvcreate, vgremove, etc.)
	# Level 2+: Trace all commands including read-only (lvs, pvs, vgs, etc.)
	case "${CMD-}" in
		lvs|pvs|vgs|vgck|vgscan)
			# Read-only commands - only trace at level 2+
			[[ "${LVM_DEBUG_LEVEL:-0}" -lt 2 ]] && RUN_DBG="" ;;
		pvcreate|pvremove|lvremove|vgcreate|vgremove)
			# Modifying commands - only trace at level 1+
			[[ "${LVM_DEBUG_LEVEL:-0}" -lt 1 ]] && RUN_DBG="" ;;
	esac

	#
	# Determine LVM binary path
	#
	# Build directory testing: Use $abs_top_builddir/tools/lvm
	# Installed testsuite: Use system 'lvm' command
	#
	if [[ -n "${abs_top_builddir-}" ]]; then
		CALL=( "$abs_top_builddir/tools/lvm" )
	else
		CALL=( "command" "lvm" )
	fi

	# Execute the LVM command with optional debug wrapper
	$EXEC $RUN_DBG "${CALL[@]}" "$CMD" "$@"
}

#
# Script execution mode handling
#
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	#
	# Direct execution mode
	#
	# Script is being run directly (not sourced).
	# Load paths and execute the requested command via wrapper.
	#
	. lib/paths

	__lvm_command_wrapper "$@"
else
	#
	# Source mode - Generate LVM command wrapper functions
	#
	# When sourced, this script dynamically creates wrapper functions for all
	# LVM commands. Each wrapper function:
	#   1. Preserves and temporarily disables 'set -x' trace mode if active
	#   2. Calls __lvm_command_wrapper with the command arguments
	#   3. Restores the original trace mode state
	#
	# This approach avoids bash fork overhead.
	#

	#
	# Discover available LVM commands
	#
	# Query available commands using '__lvm_command_wrapper help'.
	# This ensures proper path handling for both build directory and installed scenarios.
	# LVM_VALGRIND=0 disables debug tracing during command discovery.
	#
	__lvm_cmd=
	__lvm_rest=
	__lvm_cmds=( lvm )
	while read -r __lvm_cmd __lvm_rest; do
		# Filter: only commands starting with lowercase letter
		# and only if corresponding symlink exists in lib/ directory
		[[ "$__lvm_cmd" =~ ^[a-z] && -e "lib/$__lvm_cmd" ]] && __lvm_cmds+=("$__lvm_cmd")
	done < <(LVM_VALGRIND=0 __lvm_command_wrapper help 2>&1)

	#
	# Generate wrapper function for each LVM command
	#
	# Creates functions like lvm(), lvcreate(), pvs(), vgs(), etc.
	# Each function preserves trace state, calls the wrapper, and restores state.
	#
	for __lvm_cmd in "${__lvm_cmds[@]}"; do
		eval "${__lvm_cmd}() {
			{ local r=0; case \$- in *x*) r=1; set +x ;; esac; } 2>/dev/null
			__lvm_command_wrapper \"\$@\"
			local s=\$?
			{ [[ \"\$r\" -eq 0 ]] || set -x; return \$s; } 2>/dev/null
		}"
	done

	# Clean up temporary variables
	unset __lvm_cmd __lvm_rest __lvm_cmds
fi
