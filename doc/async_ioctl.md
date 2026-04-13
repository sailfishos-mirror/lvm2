# Async DM ioctl

## Why this matters

Several DM ioctls carry significant kernel-side latency that
userspace cannot reduce -- only overlap.  The kernel's device-mapper
layer uses RCU for table and map management: operations like
DM_DEV_REMOVE, DM_DEV_SUSPEND, and resume call synchronize_rcu(),
which blocks for a full RCU grace period.  The actual duration
varies with CPU count, kernel preemption model, workload, and RCU
tunables -- but on a typical multi-core server it is easily tens of
milliseconds per ioctl.  When issuing N such ioctls serially the
cost is N times that delay: removing or suspending 100 devices can
take seconds just waiting for RCU.

By issuing ioctls from multiple threads their kernel-side waits
overlap.  The caller keeps a single-threaded programming model
(submit, then drain) while the thread pool handles concurrency
behind a simple API.

## Overview

This branch adds an async submission layer to libdevmapper so that
multiple DM ioctls can fly concurrently, with a thread-pool backend
doing the actual kernel calls.

## The idea in 30 seconds

```
           submit   submit   submit
             \        |        /
         [ thread-pool  (N workers) ]
             /        |        \
          ioctl     ioctl     ioctl     <-- kernel, concurrent
             \        |        /
              drain  (main thread)
              post-process each result
```

The caller builds DM tasks as usual, but instead of `dm_task_run()`
hands them to `dm_async_submit()`.  A later `dm_async_drain()` waits
for every in-flight task to finish and runs post-processing
(udev node ops, EBUSY retries) back on the main thread.

## Public API

### Context lifecycle

```c
struct dm_async_ctx *dm_async_ctx_create(unsigned max_inflight);
void                 dm_async_ctx_destroy(struct dm_async_ctx *ctx);
```

Pass `0` for `max_inflight` to auto-size (2 x online CPUs, min 16).
The context captures the library's shared `/dev/mapper/control` fd
(opened lazily by the first `dm_task_run()` or `dm_async_ctx_create()`).

### Submit and drain

```c
int dm_async_submit(struct dm_async_ctx *ctx, struct dm_task *dmt,
                    dm_async_complete_fn complete_fn, void *userdata);

int dm_async_drain(struct dm_async_ctx *ctx, unsigned *n_inflight);
```

`dm_async_submit()` builds the ioctl buffer (via `dm_task_prepare()`)
and queues the task; the thread pool picks it up immediately.

`dm_async_drain()` has two modes:

- **Blocking** (`n_inflight == NULL`): wait until every submitted
  task has completed and been post-processed.
- **Non-blocking** (`n_inflight != NULL`): process only completions
  that are already ready, then return with `*n_inflight` set to the
  number of tasks still pending.

Worker threads only execute the raw `dm_ioctl_exec()`.  All
post-processing -- EBUSY resubmission, DM_BUFFER_FULL re-allocations,
device-node ops, udev cookie signalling, and completion callbacks --
runs on the main thread inside `dm_async_drain()`.

### Phase ordering

```c
void dm_task_set_seq(struct dm_task *dmt, unsigned seq);
```

Tasks at sequence N all run concurrently; sequence N+1 does not begin
until every N task has finished.  This gives coarse-grained ordering
without explicit dependency tracking -- useful for layered teardowns
where children must go before parents.

### Tree integration

```c
void dm_tree_set_async_ctx(struct dm_tree_node *dnode,
                           struct dm_async_ctx *ctx);

void dm_tree_set_skip_deps(struct dm_tree *dtree, unsigned skip);
```

When an async context is attached to a dependency tree,
`dm_tree_deactivate_children()` automatically defers leaf-node
removals into the pool.  Internal (non-leaf) nodes still deactivate
synchronously so that ordering is preserved by the tree walk itself.

`dm_tree_set_skip_deps()` tells the tree not to follow kernel
`DM_TABLE_DEPS` when adding nodes -- producing leaf-only trees
where each device is directly deactivatable without subtree traversal.
This is used for async thin deactivation where each thin LV should
be a leaf node eligible for async deferral, without pulling in the
shared pool subtree (which cannot be removed until all thins are gone).

## dmsetup

Multi-device `dmsetup remove` (and `suspend`, `resume`, `wipe_table`)
now wraps its loop in an async context when more than one device is
specified:

    dmsetup remove dev1 dev2 dev3 ... devN

All operations fly asynchronously; a single `dm_udev_wait()` at the end
covers the batch.  The `--noasync` flag forces the old sequential
behaviour for debugging.

Concise creation (`dmsetup create --concise`) benefits too -- each
device in the batch is created asynchronously.

### remove_all considered slow

The built-in `dmsetup remove_all` iterates devices inside the kernel
one by one, issuing a synchronous remove for each.  With async
`dmsetup remove`, a simple shell one-liner can already outperform it:

    dmsetup remove $(dmsetup info --noheadings -c -o name)

This lists every device, then removes them all in one async batch.
Devices with dependencies will get EBUSY and retry automatically,
so ordering does not need to be figured out in advance.

A smarter built-in `remove_all` that queries the dependency graph
and tears down leaves first (in async batches per tree level)
could be implemented on top of the async API -- but the shell
version above is already a practical improvement.

## LVM integration

`lvchange --activate n -S ...` (or any bulk deactivation) goes through
`setup_async_lvs()` in toollib, which decides whether async
ioctl is worthwhile:

- 4+ simple active LVs (linear/striped, no snapshots) in one VG, or
- multiple thin volumes sharing a pool.

Eligible LV UUIDs are stored in a radix tree (`cmd->async_lv_set`)
rather than mutating LV status flags.  This preserves const-ness of
the committed VG copy -- both the live and committed LV structs share
the same UUID, so a single lookup works for either.

In `dev_manager.c`, `_lv_is_async()` checks the radix tree to route
each LV through the async or synchronous path.

When the threshold is met, an async context is created on `cmd->async_ctx`
and flows down through `deactivate_lv()` into the deptree layer.
After all LVs have been processed, `finish_async_lvs()` destroys
the radix tree, drains the context, and waits for udev.

A configuration knob controls the feature:

    activation {
        use_async_ioctl = 1   # default on
    }

## Design notes

**Main-thread post-processing.**  Worker threads only execute the
ioctl.  Everything else -- device-node operations, udev cookie
signalling, completion callbacks -- runs on the main thread during
drain.  This keeps libdevmapper's node-ops code (which was never
designed for concurrency) untouched.

**EBUSY retry.**  A dedicated retry thread sleeps on failed removals
and re-queues them after a short delay, so busy workers are never
blocked waiting.

**DAG safety.**  A `removal_queued` flag on each tree node prevents
the same device from being removed twice when it is reachable through
multiple parents in the dependency graph.

## Branch structure

The branch is organised in three tiers.

### Core (mandatory)

The preparatory refactors, the async API, thread-pool backend,
deptree integration, dmsetup async paths, and the LVM toollib
glue.  This is the complete, self-contained feature -- everything
needed for async deactivation to work.

### Experimental (MAYBE)

Ideas that are not required for the core feature and could be added
independently later:

- **Synchronous single-slot backend** -- a trivial async backend
  that executes inline, no threads.  Useful for testing the async
  code paths without actual concurrency, or as a fallback on systems
  where thread creation is undesirable.

- **Seq-based phase barrier** -- `dm_task_set_seq()` support in the
  thread pool, allowing coarse-grained ordering of async batches.
  The core feature works fine without it (everything runs at seq 0).

- **Thread-safe suspended counter and node-ops list** -- mutex
  protection for the global suspended-device counter and the
  per-task node-ops list.  Only matters if post-processing ever
  moves off the main thread; currently it does not.

- **/proc scan on EBUSY** -- debug aid that scans `/proc` to log
  which processes hold a device open when `DM_DEVICE_REMOVE` gets
  `EBUSY`.  Developer-only; guarded behind a compile-time define.
  Too slow for production but invaluable when chasing a holder
  that refuses to let go.

### Dropped (io_uring)

The branch contains a prototype `io_uring` backend that would
replace the thread pool with kernel-side async dispatch via
`IORING_OP_IOCTL`.  The kernel opcode does not exist upstream --
it was a speculative experiment.  These commits are kept for
reference but will not be merged:

- `configure: check for nonexisting IORING_OP_IOCTL`
- `libdm: wishful io_uring async backend`

If `IORING_OP_IOCTL` (or a similar mechanism) ever lands in the
kernel, the backend can be resurrected.  The async API was designed
with pluggable backends precisely for this reason -- swapping
thread pool for io_uring requires no changes above the vtable.
