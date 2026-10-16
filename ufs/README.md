# UFS 4.1 / 5.0 Example Configurations

Example configs and workloads for MQSim's UFS host interface (MCQ mode, UFSHCI 4.1/5.0).
Design documentation: `../ufs-design.md`. Implementation plan and spec grounding: `../plan.md`.

## Files

| File | What it is |
|---|---|
| `ssdconfig-ufs-4.1.xml` | UFS 4.1 device on the stock flash backend. HS-GEAR5 (auto), Rate B, 2 lanes, spec-default MAC=32. |
| `ssdconfig-ufs-5.0.xml` | Same device as UFS 5.0: only `UFS_Version` differs (HS-GEAR6 auto). The MCQ register layer is identical in both versions per the JESD223G changelog, so the modeled 5.0 delta is link speed. |
| `ssdconfig-ufs-4.1-fastflash.xml` | 4.1 on a faster flash backend (800 MT/s channels, 25 µs tR ≈ 5.7 GB/s ceiling), 1 lane, MAC=256. Makes the *link* the binding constraint. |
| `ssdconfig-ufs-5.0-fastflash.xml` | The 5.0 counterpart. Run both on the sequential workload to see the 4.1→5.0 bandwidth doubling directly. |
| `workload-ufs-sequential.xml` | 4 flows × 256 KB sequential reads, QD16. The link-bound demo workload. |
| `workload-ufs-multiqueue.xml` | 16 flows × 4 KB random reads, QD16. Exercises MCQ with 16 SQs (past the 8-queue ceiling of MQSim's NVMe model). |

## The headline demo: UFS 4.1 vs 5.0

```
./MQSim -i ufs/ssdconfig-ufs-4.1-fastflash.xml -w ufs/workload-ufs-sequential.xml
./MQSim -i ufs/ssdconfig-ufs-5.0-fastflash.xml -w ufs/workload-ufs-sequential.xml
```

Expected (validated results): 4.1/HS-G5 ≈ **2.26 GB/s** aggregate at ~7.3 ms mean response.
5.0/HS-G6 ≈ **4.52 GB/s** at ~3.7 ms, a 2.0× bandwidth ratio at 0.5× response, both gears running
at ~97% of their link caps (the remainder is modeled protocol overhead: two-step UTRD+UCD command
fetch, Response UPIU + CQ entry write-backs, 24 B DATA UPIU headers per 64 KB).

On the **stock** backend (`ssdconfig-ufs-4.1.xml` vs `-5.0.xml`) the same comparison shows ~no
difference. The stock flash array (~1.76 GB/s) is slower than even a 1-lane HS-G5 link, so the
device is flash-bound and gains nothing from a faster link. That is faithful physics, not a bug.
Gear upgrades only pay when the link is the bottleneck.

## Knob notes (host `<Host_Parameter_Set>` section unless stated)

- `UFS_Version` (`4.1` | `5.0`): sets the HS-Gear ceiling and the auto-gear default. Explicit
  `UFS_Gear` values above the version's ceiling are config errors (HS-GEAR6 requires UFS 5.0 /
  M-PHY 6.0).
- `UFS_Gear` (0-6, default 0 = auto): auto resolves to the version's mandatory maximum
  (4.1→G5, 5.0→G6). Exact per-lane rates from JESD220G/H Table 6.7.
- `UFS_Rate_Series` (`A` | `B`, default B), `UFS_Lane_Count` (1-2, default 2).
- `UFS_Link_Efficiency` (default 0.80): effective-throughput factor on the raw PHY rate
  **[assumption: the specs state no line-coding efficiency figure]**.
- `UFS_Processing_Delay` (default 400 ns): HBA UTP/UniPro stack latency **[assumption, by analogy
  with `SATA_Processing_Delay`]**.
- Device section: `UFS_Max_Active_Commands` (MCQConfig.MAC analogue, spec reset default 32) caps
  device-wide outstanding commands. The default is faithful to the spec but throttles multi-queue
  scaling: with 16 QD16 flows, expect an aggregate-IOPS plateau at MAC=32 and NVMe-matching scaling
  at MAC=256. `UFS_Completion_Queue_Count` (0 = one CQ per SQ) gives N:1 SQ→CQ mapping when
  smaller than the flow count. `UFS_Arbitration` (`RR` | `STRICT_PRIORITY`) selects MCQConfig.AS.
  Strict priority uses the flow `Priority_Class` as SQPL and, per spec, has no anti-starvation.
  `UFS_IACTH` / `UFS_IATOVAL_us` (defaults 0 = off) enable count/timeout interrupt aggregation.
- `IO_Queue_Depth` must be ≤ 8192 for UFS (SQATTRy.SIZE ceiling, one ring slot is reserved).
  NVMe-style 65535 is rejected at validation.
- UFS response-time statistics clock from ring enqueue (after the HBA submit pipeline), so under
  bursty arrival they should be read together with the end-to-end delay, which clocks from request
  arrival. Same convention as the SATA HBA.
