# Testing UFSSimulator

This folder holds the regression checks for UFSSimulator, the UFS-enabled fork of MQSim.

## Running the regression checks

```
./tests/regression/run_regression.sh
```

The script builds MQSim, then checks two things.

1. The NVMe workload's scenario 1 result XML matches the golden reference
   committed under `tests/regression/golden/workload_scenario_1.xml`. A
   mismatch means the NVMe or SATA data path changed behavior. That needs an
   explanation before the golden file gets updated to match.
2. The UFS workload's scenario 1 result XML matches the golden reference at
   `tests/regression/golden/workload_scenario_1_ufs.xml`, using the config at
   `tests/regression/ssdconfig-ufs.xml`. A mismatch means UFS behavior
   changed. As more of UFS gets implemented, this golden file is expected to
   be updated deliberately (not just regenerated to make the check pass) each
   time UFS behavior changes for a real reason.

## Checking the EXTERNAL GC policy hook

```
./tests/external_gc_policy/run_test.sh
```

`GC_Block_Selection_Policy=EXTERNAL` hands each GC block-selection decision to
an outside controller over a pair of named pipes instead of resolving it from
one fixed policy. This script builds MQSim, then plays that controller
itself: it drives the pipes directly, switching the selected action on every
single decision, and stops with an explicit error (printing the captured
log) if any run exits non-zero. Switching the action every decision is deliberate. It is
what makes the race described in plan.md section 8 item 9 likely enough to
hit, and is how that race was originally found.

## Checking the GC write-completion race (RANDOM_PP)

```
./tests/gc_write_completion_race/run_test.sh
```

`RANDOM_PP` used to crash the GC-stress fixture deterministically, on its
own, with no live hook or pipes involved. This script just builds MQSim and
runs it against that fixture natively with `GC_Block_Selection_Policy=RANDOM_PP`,
checking for a clean exit. Confirmed to catch the regression: run against
the commit before the fix in plan.md section 8 item 11, it crashes partway
through with the same "Inconsistency in the global mapping table when
locking an LPA!" error RANDOM_PP always hit.

## Checking the preconditioning RANDOM_UNIFORM crash

```
./tests/preconditioning_random_uniform/run_test.sh
```

`Enabled_Preconditioning=true` with the `RANDOM_UNIFORM` address
distribution used to crash with a SIGSEGV during preconditioning. This
script builds MQSim and runs it against the device and workload that
crashed deterministically, checking for a clean exit. Slow on purpose:
preconditioning alone takes a couple of minutes on a device this size, so
it isn't part of the fast default suite and needs running deliberately.
Confirmed to catch the regression: run against the commit before the fix
in plan.md section 8 item 6, it exits 139 (SIGSEGV) partway through
preconditioning.

## Running the UFS unit/integration tests

```
./ufs/tests/run_tests.sh
```

Links the compiled `.o` files directly (everything except `main.o`) against
four small test binaries that call the UFS host interface's internal APIs
in-process, instead of running the full simulator end to end:
`test_construction` (ring construction), `test_arbitration` (arbitration and
MAC), `test_e2e` (the two-step fetch path end to end), and `test_completion`
(completion and interrupt aggregation). Each one gets its own process, since
they share the engine singleton.

## A build pitfall that produces wrong results instead of a build error

A build that stops partway, for example a failed link step or an
interrupted `make`, can leave stale object files in `build/`. Relinking
those stale objects after a toolchain change produces a binary that links
and runs, but silently mixes object code built against two different header
versions. The symptom is not a crash. It is a simulation that completes
normally but reports different request counts or response times than
expected.

If a regression check fails and nothing in the source actually changed, run
`make clean` and rebuild before assuming the code is at fault.

## Trace-based workloads and datasets

`traces/wsrch-small.trace` and `traces/tpcc-small.trace` are already
committed. They are enough to run the regression checks and the
trace-based scenario in the bundled `workload.xml`. Their exact origin is
not recorded in this repository.

Trace files follow the ASCII format from the DiskSim Simulation
Environment, described in the main README's "Defining a Trace-based
Workload" section and in reference [8] there: one request per line, with
arrival time, device number, starting logical sector address, request size
in sectors, and read or write type, in that order.

Larger or different storage traces in a compatible format can be found at
public research trace repositories, including the UMass Trace Repository
(https://traces.cs.umass.edu) and the SNIA IOTTA Repository
(https://iotta.snia.org). A downloaded trace needs converting to the
five-field line format above before `IO_Flow_Parameter_Set_Trace_Based` can
read it, unless it already uses that format.
