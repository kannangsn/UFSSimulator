# Testing UFSSimulator

This folder holds the regression checks for UFSSimulator, the UFS-enabled fork of MQSim.

## Running the regression checks

```
./tests/regression/run_regression.sh
```

The script builds MQSim, runs the pre-existing NVMe workload, and checks two things.

1. Scenario 1's result XML matches the golden reference committed under
   `tests/regression/golden/`. A mismatch means the NVMe or SATA data path
   changed behavior. That needs an explanation before the golden file gets
   updated to match.
2. A UFS-configured run still exits with an explicit error where `SSD_Device`
   would otherwise construct `Host_Interface_UFS`, with the exact message the
   placeholder code prints. This check exists because UFS support is still
   being built. It needs to change once `Host_Interface_UFS` and `UFS_HBA`
   actually process requests, at which point the check should assert success
   instead of an explicit failure.

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
