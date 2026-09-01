# OVERVIEW

lvmtest is an LVM cluster test framework that runs test scripts across multiple
VMs representing different cluster nodes.

- cluster/configs/ contains test cluster config specs, used to create
  clusters with specific properties (e.g. number of VMs, VM properties,
  OS image to use, number, size and types of devices.)
- "lvmtest" provides the front-end user interface for creating clusters
  and running test scripts on clusters.
- "lvmtest create -c configs/test-cluster.conf" creates a new cluster of
  VMs with shared storage.
- "lvmtest create" creates N nodes: node1, node2, ..., nodeN, plus a special "node0"
- node0 is the storage-exporter node; it exports devices (e.g. via iscsi) to the all
  the test nodes (node1..nodeN).  Because the same devices are exported from node0
  to all the test nodes, the devices act as shared storage (like a SAN -
  Storage Area Network), and can be used to test shared VGs.
- cluster/shell/, and subdirs, contain test scripts, e.g. shell/example.sh
- "lvmtest run -t shell/example.sh" runs the example.sh test on the running cluster.
- example.sh contains a series of lvm commands to run on test nodes.
- preceding each lvm command in example.sh is an indication of which node
  the command should be run on. e.g. "node1 <command>" runs <command> on node1.
  "node2 <command>" runs it on node2, etc.
- Special node designations are:
  "nodep <command>": run command on all nodes in parallel.
  "nodes <command>": run command on all nodes sequentially.
  "noden <num> <command>": run command on node <num>.
  "success_node <command>": used after nodep to run command on the
   single node which successfully completed a nodep command.
- "not <command>" indicates command is expected to fail.
- example.sh fails if a command, or other check, fails when it is expected to succeed.
- results of each test are stored under cluster/results/.  This
  includes a log of commands execuated and their output, and debug info collected
  from VMs after a test fails.
- lvmtestssh and lvmtestscp are command line tools to easily ssh and scp
  to test node VMs.
- "lvmtest revert" resets all the VMs to a snapshot that was taken after
  they were first created.  This can be used to reset the cluster nodes
  to a clean state after each test.
- When testing shared VGs, tests focus on ensuring that lvm locking, through
  lvmlockd and the lock manager (sanlock or dlm), is correctly doing mutual
  exclusion for lvm commands on multiple nodes using the same VG.
- lvmtest can use "bridge mode" to run old style lvm test scripts (from ../shell/)
  on just one node.  In this way, the old style test scripts run against a VM's
  standard devices rather than virtual devices that they traditionally use.

Host prerequisites for non-root usage: see skill `lvmtest-setup`.
Device-type testing conventions (SCSI/NVMe/multipath): see `shell/CLAUDE.md`.
  
