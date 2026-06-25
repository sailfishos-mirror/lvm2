# OVERVIEW

The test directory contains two separate testing frameworks for LVM:

1. The virtual-device test framework, with the make/Makefile front end.
   Located primarily in directories: api/ lib/ shell/

   This framework is runs test scripts that run LVM commands on the local
   host machine.  The LVM commands run against fake devices that are really
   dm devices backed by ramdisk or loop devices set up on the host system.
   LVM commands run by this framework are highly configured so that they run
   using custom file locations separate from the local system.

2. The virtual-machine test framework, with the lvmtest script front end.
   Located under directory: cluster/

   This framework creates a cluster of VMs, and runs test scripts across
   all of the cluster nodes (VMs).  LVM commands are not run on the host
   system.  The test scripts run LVM commands which use the VM's standard
   devices.  The VM's devices are imported from a special storage-exporter
   VM (node0) which exports scsi/nvme devices to all of the test VMs.

Both test frameworks use similar test scripts, which are largely a series
of lvm commands to execute.  The main diffference is that lvmtest script
commands are all preceded by "nodeN" indicating which cluster node the
command should run on.  The scripts use command exit codes to determine
if each step in a test script is successful.

