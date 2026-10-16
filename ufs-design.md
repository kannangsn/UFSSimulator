# UFS MCQ Design for MQSim: Registers, Descriptors, Queues

**Phase A / Day 1 deliverable** (see `plan.md` §7). This is the living design document that the
implementation follows. `plan.md` says *what* and *when*. This file says *how*. Basis:
JESD223F/G for MCQ mechanics (`plan.md` §2.4), JESD220G/H for UPIU formats (`plan.md` §2.1).
Everything here is MCQ-only. There is no legacy single-doorbell mode (`plan.md` §5.1). A
`UFS_HBA` owns the host-side queues (`plan.md` §5.2).

Every deliberate deviation from spec-literal behavior is tagged **[ABSTRACTION]** (timing-neutral
simplification) or **[ASSUMPTION]** (a constant the spec doesn't provide, per `plan.md` §8).

---

## 1. Address map

MQSim invents its own flat address space per protocol (NVMe: register block at `0x1000`, ring
buffer memory at `q << 40`, data at `DATA_MEMORY_REGION = 0xFF0000000000`). UFS gets its **own register
block at `0x2000`**, deliberately disjoint from NVMe's `0x1000` block per `plan.md` §6. It
reuses the ring-buffer-memory encoding shape, which is safe because only one host-interface type is
active per simulation.

### 1.1 Device-side doorbell registers (written via `PCIe_Root_Complex::Write_to_device`)

| Register | Address | Written by | Value |
|---|---|---|---|
| `UFS_SQ_TAIL_DOORBELL(q)` | `0x2000 + q*0x10` | host (HBA), after enqueuing UTRD(s) into SQ ring `q` | new SQ tail slot index |
| `UFS_CQ_HEAD_DOORBELL(c)` | `0x2008 + c*0x10` | host (HBA), after consuming CQ entries from CQ ring `c` | new CQ head slot index |

- `q`, `c` ∈ 0..31, so the block spans `0x2000`–`0x21FF`. Queue numbering starts at 0.
  **UFS has no admin-queue reservation** (that's an NVMe concept. UFS device management is Query
  Requests, which are out of scope). Unlike NVMe's `FLOW_ID_TO_Q_ID(f) = f+1`, UFS uses
  `SQ id = flow id` directly.
- **[ABSTRACTION]** Doorbell values are slot indices, not the spec's DWord offsets, and registers
  sit at fixed simulator-chosen addresses rather than behind `SQDAOy`/`CQDAOy` indirection. Both
  spec mechanisms are hardware-discoverability features. A doorbell write is one small MMIO write
  either way, so timing is unaffected.

### 1.2 Host-memory regions (read/written via `PCIe_Root_Complex::Read_from_memory` / `Write_to_memory`)

| Region | Address | Contents |
|---|---|---|
| SQ ring `q` | `(uint64_t)q << 40` | `Queue_Size` × 32-byte UTRD slots |
| UCD area, SQ `q`, slot `s` | `((uint64_t)q << 40) \| (0x1ULL << 36) \| (s * 0x100)` | Command UPIU (32 B) + PRDT (16 B). Response UPIU written back at `+0x40` |
| CQ ring `c` | `((uint64_t)c << 40) \| (0xFULL << 36)` | `Queue_Size` × 32-byte CQ-entry slots |
| Data | `DATA_MEMORY_REGION` (unchanged) | dummy data addresses, as NVMe/SATA already use |

- Decode rule (device side): `queue_or_cq_id = address >> 40`. Bits 39:36 discriminate SQ ring
  (`0x0`), UCD area (`0x1`), CQ ring (`0xF`). Mirrors how NVMe decodes `address >> NVME_COMP_Q_MEMORY_REGION`.
- The UCD region is real and separately addressed because UFS command fetch is **two-step**
  (see §3). The UTRD does not contain the command inline, unlike NVMe's SQE. This is a genuine
  protocol difference with link-timing consequences, so it is modeled, not flattened.

## 2. Structures carried over the link

### 2.1 UTRD: SQ entry (8 DWORDs, 32 bytes, JESD223F §6.1.1 / §6.2.1)

```cpp
struct UTP_Transfer_Request_Descriptor   // = MCQ SQ entry, verbatim layout
{
    uint32_t DW0_Header;        // Command Type (31:28), DD (26:25), I (24), CCI (7:0)
    uint32_t DW1_DUN_Lower;     // crypto Data Unit Number, carried but never interpreted [see §7]
    uint32_t DW2_OCS;           // OCS is in the CQ entry in MCQ mode, kept here for layout fidelity, written 0x0F (invalid) as the spec requires when unused
    uint32_t DW3_DUN_Upper;
    uint32_t DW4_UCD_Base_Lower;   // points into the UCD area (§1.2)
    uint32_t DW5_UCD_Base_Upper;
    uint32_t DW6_Response_UPIU;    // offset (31:16), length (15:0), in DWords
    uint32_t DW7_PRDT;             // offset (31:16), length in entries (15:0)
};
```

### 2.2 CQ entry (8 DWORDs, 32 bytes, JESD223F §6.2.2, Figure 14)

```cpp
struct UFS_Completion_Queue_Entry
{
    uint32_t DW0;   // UCDLBA (31:7) | SQID (4:0), SQID routes the completion back to its flow
    uint32_t DW1;   // UCD base upper
    uint32_t DW2;   // Response UPIU offset (31:16) | length (15:0)
    uint32_t DW3;   // PRDT offset (31:16) | length (15:0)
    uint32_t DW4;   // Extended Error Code (15:8) | OCS (7:0), authoritative status in MCQ mode
    uint32_t DW5;   // EXT_IID (23:20) | IID (19:16) | LUN (15:8) | Task Tag (7:0)
    uint32_t DW6_Reserved;
    uint32_t DW7_Reserved;
};
```

### 2.3 UCD contents: Command UPIU (32 B) + PRDT (16 B), Response UPIU (32 B)

Command UPIU per JESD220G §10.7.1 (12-byte basic header + Expected Data Transfer Length + 16-byte
CDB inline). The simulator populates/parses only: Transaction Type, LUN (= stream/flow id), Task
Tag, Command opcode + LBA + transfer length inside the CDB area, Expected Data Transfer Length.
Everything else is carried as bytes for size fidelity but never interpreted.

- **[ABSTRACTION]** One 16-byte PRDT entry per request with dummy `DATA_MEMORY_REGION` addresses,
  the same device-DMA modeling NVMe uses with its dummy PRP pointers. PRDT length participates in
  the UCD fetch size. Scatter/gather fan-out is not modeled (a single MQSim request is one
  contiguous extent by construction).
- Response UPIU (32 B) is written back to the UCD response region as part of completion (§3),
  so its link cost is modeled.

### 2.4 Task tags

Per-SQ pool of 8-bit tags (0–255), allocated at submission, freed at completion. This is the UFS
analogue of NVMe's `available_command_ids`, but **per queue** and 8-bit wide (JESD220G: Task Tag
is 1 byte). Consequence: at most 256 outstanding commands per SQ regardless of ring depth. With
the default `MAC = 32` device-wide cap (§5) this never binds in practice, but the invariant is
enforced where tags are allocated, not assumed.

## 3. Command flow: what crosses the link, and when

Submission (flow → HBA → device):

1. Flow submits `Host_IO_Request` → `UFS_HBA::Submit_io_request` (adds `UFS_Processing_Delay`, §5).
2. HBA allocates a task tag + SQ slot (stall/waitlist if ring full or no tag, like NVMe's
   `waiting_requests`), builds UTRD + UCD in ring memory, writes **SQ tail doorbell** (§1.1).
3. Device (`Request_Fetch_Unit_UFS`) sees the doorbell, arbitrates across non-empty SQs (§4),
   and issues DMA read #1: **UTRD fetch, 32 B** from the SQ ring.
4. DMA read #2: **UCD fetch, 48 B** (Command UPIU 32 B + PRDT 16 B) from the UCD area.
   This second round trip is the UFS-vs-NVMe structural difference (NVMe: one 64 B SQE fetch).
5. Command parsed → `User_Request` → existing cache/FTL pipeline, unchanged.
6. Data phase: DATA IN/OUT transfers modeled with the **24-byte per-UPIU header overhead**
   (`plan.md` §2.1) added to payload size on the link.

Completion (device → HBA → flow):

7. Device writes **Response UPIU, 32 B**, to the UCD response region.
8. Device writes the **CQ entry, 32 B**, into CQ ring `CQID(q)`. OCS is here (§2.2).
   Steps 7+8 are issued as two separate `Write_to_memory` deliveries (`Send_completion_entry`
   in `Request_Fetch_Unit_UFS`), not one aggregate 64 B message. Both transfers' link occupancy
   is modeled, but only the CQ entry is behaviorally consumed by the host.
9. Interrupt aggregation gate (§6): the HBA consumes the CQ entry when the IACTH/IATOVAL
   condition fires (or immediately when aggregation is disabled).
10. HBA maps `SQID`+Task Tag → original request, frees tag + slot, advances CQ head, writes
    **CQ head doorbell**, routes completion to the owning flow (SATA-style
    `UFS_consume_io_request`).

Per-command link budget vs. NVMe, holding data payload equal: UFS moves 32+48+32+32 = 144 B of
descriptors/UPIUs + 24 B per data UPIU header, where NVMe moves 64 B SQE + 16 B CQE = 80 B.
This overhead asymmetry is precisely what the simulator is being built to expose.

## 4. Arbitration (device side, `Host_Interface_UFS`)

Global scheme selector mirroring `MCQConfig.AS` (`plan.md` §2.4), config knob `UFS_Arbitration`:

- **RR** (default): round-robin pointer over SQs with pending doorbell work.
- **STRICT_PRIORITY**: serve the lowest-`SQPL` nonempty SQ first, RR among equals. `SQPL` is
  derived from the flow's existing `Priority_Class` (URGENT→0, HIGH→1, MEDIUM→2, LOW→3,
  UNDEFINED→7). The 8-level SQPL space is exposed in the struct but only 4 levels plus 7 are
  producible from MQSim workloads. Documented, not hidden.
- Backend pairing: UFS configs use the non-priority `TSU_OutofOrder`. Arbitration stays at the
  host interface. No mapping onto NVMe's weighted classes.
- Fetch pacing: per-SQ in-flight window reuses `Queue_Fetch_Size` semantics from NVMe, and the
  device-wide `MAC` cap (§5) is enforced on top.

## 5. Configuration knobs

XML names follow existing `Device_Parameter_Set` / `Host_Parameter_Set` conventions.

| Knob | Home | Default | Range / notes |
|---|---|---|---|
| `HostInterface_Type` | Device | n/a | new value `UFS` |
| SQ count | n/a (implicit) | = number of IO flows | ≤ 32 (spec MAXQ), validated at parse |
| `UFS_Completion_Queue_Count` | Device | = SQ count (1:1) | 1..SQ count, SQ *i* → CQ *(i mod count)* gives N:1 topology |
| `IO_Queue_Depth` (reused) | Device | 1024 | ring slots per SQ and per CQ, spec max 8192, one slot always reserved (§2.4 ring convention) |
| `Queue_Fetch_Size` (reused) | Device | 512 | per-SQ in-flight fetch window |
| `UFS_Max_Active_Commands` | Device | 32 | `MCQConfig.MAC` analogue, device-wide outstanding cap (spec default 0x1F = 32) |
| `UFS_Arbitration` | Device | `RR` | `RR` \| `STRICT_PRIORITY` (§4) |
| `UFS_IACTH` | Device | 0 (disabled) | 0..31 completions per interrupt-aggregation flush (§6) |
| `UFS_IATOVAL_us` | Device | 0 (disabled) | aggregation timeout, **defined in µs directly**. The spec's 40 µs-unit encoding is a legacy-register analogy, not restated for MCQ (`plan.md` §2.4), so we avoid inheriting its unit ambiguity |
| `UFS_Version` | Host | `4.1` | `4.1` \| `5.0`, the modeled standard version. Sets the gear ceiling (G5 vs G6) and the auto-gear default. The MCQ/register path is identical for both (JESD223G changelog, `plan.md` §4.2), the version's modeled effect is link speed only. |
| `UFS_Gear` | Host | 0 (auto) | 0 = the version's mandatory maximum (4.1→G5, 5.0→G6, per JESD220G/H §8.4), explicit 1..6 validated against `UFS_Version` at link construction. G6 on a 4.1 config is a hard error (requires M-PHY 6.0) |
| `UFS_Rate_Series` | Host | `B` | `A` \| `B` (Rate-B @ 19.2/38.4 MHz refclk column) |
| `UFS_Lane_Count` | Host | 2 | 1..2 |
| `UFS_Link_Efficiency` | Host | 0.80 | **[ASSUMPTION]** 8b10b line-coding factor, no spec-stated effective-throughput figure exists (`plan.md` §8 item 2) |
| `UFS_Processing_Delay` | Host | 400 ns | **[ASSUMPTION]** HBA/UTP stack latency, `SATA_Processing_Delay` analogue (same default) |

Link instantiation (in `Host_System`'s UFS branch): reuse `PCIe_Link` with
`lane_bandwidth_GBPs = rate_Mbps × efficiency / 8000`, `lane_count = UFS_Lane_Count`, and packet
overhead **[ASSUMPTION]** `header 8 B / max payload 256 B` approximating UniPro L2 framing in
place of PCIe's TLP 20 B / DLLP 6 B / 128 B (`plan.md` §6 "Link timing"). Worked default:
HS-G5 Rate-B = 23347.2 Mbps × 0.80 / 8000 ≈ **2.33 GB/s/lane**. Gear 6 doubles it.

## 6. Interrupt aggregation (built fresh, no precedent implemented in this codebase)

One aggregation context per CQ (**[ABSTRACTION]**: IAG groups collapse to per-CQ contexts, since
grouping CQs is only meaningful for interrupt-line sharing, which MQSim doesn't model). Device
posts CQ entries to ring memory as they complete (§3 step 8), but the HBA-visible completion
event is gated:

- counter reaches `UFS_IACTH` → flush, or
- oldest pending entry age reaches `UFS_IATOVAL_us` → flush (one timer event per CQ context on
  the simulator engine), or
- both knobs 0 → immediate delivery (NVMe-equivalent behavior, the validation baseline).

Flush = deliver all pending CQ entries to the HBA consumption path (§3 steps 9-10).

## 7. Carried-but-never-interpreted / out of scope

Struct fields kept for byte-layout fidelity but never read by simulator logic: crypto (CCI/DUN),
EHS lengths, IID/EXT_IID (always 0), Data Direction bits beyond read/write classification.
Out of scope entirely, per `plan.md` §2.5/§4: Query Request content, Task Management, power modes,
inline hashing, ZUFS.

**RPMB, decided (Phase B Day 9): stubbed, not modeled.** MQSim workloads cannot express RPMB or
security commands at all. `Host_IO_Request` carries only READ/WRITE types, and both trace and
synthetic flow generators produce nothing else, so the ≥2-round-trip authenticated-command
latency model (`plan.md` §3) has no traffic to attach to. If RPMB-bearing workloads are ever
added, the multiplier belongs in the command-parse path of `Request_Fetch_Unit_UFS`
(a SECURITY PROTOCOL IN/OUT opcode would issue a second command round trip before completion,
serialized per RPMB region). Documented as a known simplification.

## 8. Validation invariants (checked in `Validate_simulation_config` / config parse)

1. UFS mode ⇒ flow count ≤ 32, `UFS_Completion_Queue_Count` ≤ flow count.
2. `IO_Queue_Depth` ∈ [2, 8192]. Usable slots = depth − 1 (reserved-slot ring convention).
3. `UFS_Max_Active_Commands` ≥ 1. Outstanding-per-SQ ≤ min(depth−1, 256 tags, `Queue_Fetch_Size`).
4. Existing NVMe/SATA configs must be untouched by construction. No shared constant in
   `Host_Defs.h`/`Host_Interface_Defs.h` is modified, only added to (regression gate).
