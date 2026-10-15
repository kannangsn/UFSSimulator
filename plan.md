# Adding UFS 4.1 / UFS 5.0 Support to MQSim

**Architecture gap analysis and a day-by-day implementation plan**

- Repository: MQSim (SSD simulator)
- Baseline reference (original plan): JEDEC JESD220 (UFS 1.0/1.1 base architecture, 2011), plus web-search summaries of UFS 4.1/5.0 deltas
- **Revised reference (this iteration): the actual current specs.** JESD220G (UFS 4.1 device/transport spec, Dec 2024), JESD220H (UFS 5.0 device/transport spec), and JESD223F (UFSHCI 4.1 host-controller-interface spec, Jan 2025, the document that actually defines MCQ/doorbell/UTRD. This was **not** covered by JESD220 at all and had to be sourced separately). See §8 for exactly which claims are spec-cross-checked versus still assumption.
- Prepared: 2026-07-15. **Revised: 2026-07-16**, after reading the real UFS 4.1/5.0 spec PDFs.

## Scope decisions (unchanged from original plan)

- **Fidelity target: performance-simulation fidelity.** Model UFS's timing-relevant behavior (queue/doorbell semantics, UPIU header overhead, HS-Gear bandwidth/latency) at the same abstraction level MQSim already uses for NVMe/SATA. Full SCSI CDB parsing, task management functions, query request *content*, and the power-mode state machine are explicitly out of scope.
- **Versioning: sequential milestones.** A working UFS 4.1 baseline (including MCQ) first, then UFS 5.0 as a smaller follow-on delta (HS-Gear 6 bandwidth constants) on the same code path.
- **Timeline: ~18 working days** (15-20 day target band), not necessarily consecutive. This band is unchanged post-analysis. MCQ turned out simpler than NVMe's arbitration in one respect (no weighted-round-robin), but added a few genuinely new modeling items (RPMB round-trip cost, interrupt aggregation) that roughly offset it.

---

## 1. Architecture Summary (unchanged)

MQSim's host-interface layer is cleanly separated from flash-level simulation via `Host_Interface_Base`, which every protocol (currently NVMe and SATA) subclasses. The base class owns a `PCIe_Switch` transport handle, a `Data_Cache_Manager_Base`, and two protocol-supplied collaborators: an `Input_Stream_Manager_Base` (segments `User_Request`s into `NVM_Transaction_Flash_RD/WR` objects) and a `Request_Fetch_Unit_Base` (drives simulated DMA of command/data payloads over the link).

NVMe models up to 8 SQ/CQ pairs via fixed MMIO doorbell addresses, with priority-weighted arbitration (URGENT/HIGH/MEDIUM/LOW). SATA is structurally a single-stream instance of the same NVMe-style queue-pair mechanics. It reuses NVMe's `Submission_Queue_Entry` struct and opcode constants directly. There is no distinct SATA transport or PHY model. Both share the same `PCIe_Link` / `PCIe_Root_Complex` / `PCIe_Switch` transport, which branches on a two-value `HostInterface_Types` enum.

The flash-level stack (`Address_Mapping_Unit_*`, `FTL`, `GC_and_WL_Unit_*`, `Flash_Block_Manager_*`, the ONFI/NVM-chip backend, and the `NVM_Transaction_Flash_RD/WR` / `User_Request` types) takes only generic stream-id, LPA, and PPA parameters with zero NVMe/SATA-specific logic. **Adding UFS is purely a host-interface and transport-layer addition. No flash-simulation code needs to change.**

## 2. UFS Protocol Model, now cross-checked with the spec text

Two separate JEDEC document series matter, and this was the biggest correction from the original (2011-spec-based) plan. **JESD220 defines the device/transport protocol** (UPIU format, SCSI command mapping, feature attributes). **JESD223 (UFSHCI) defines the host-controller register interface**: doorbells, UTRD, and MCQ are present here, not in JESD220. The original plan mixed the two together.

### 2.1 UPIU packet format (JESD220G §10.6-10.7, confirmed unchanged in JESD220H)

- **Basic/general header: fixed 12 bytes** at the start of every UPIU. Byte 0 Transaction Type, byte 1 Flags, byte 2 LUN, **byte 3 Task Tag (1 byte)**, byte 4 IID/Command Set Type, byte 5 EXT_IID/Query Fn/Task Mgmt Fn, byte 6 Response, byte 7 EXT_IID/Status, byte 8 Total EHS Length, byte 9 Device Information, bytes 10-11 Data Segment Length.
- **COMMAND UPIU: 32 bytes fixed** (12-byte header, 4-byte Expected Data Transfer Length, 16-byte CDB embedded inline), optional EHS beyond byte 31.
- **DATA OUT / DATA IN UPIU: 24-byte header** (12-byte basic, 4-byte Data Buffer Offset, 4-byte Data Transfer Count, 4 bytes of hint/reserved fields), followed by the data segment payload.
- **General UPIU size bounds (from JESD220H): minimum 32 bytes, maximum 65600 bytes.**
- No new UPIU transaction types and no header-layout changes between 4.1 and 5.0, confirmed by inspection of both specs, not just the changelog. Treat UPIU overhead modeling as version-independent.

### 2.2 Task tag and queue depth (JESD220G §13.4.13)

- Task Tag is a 1-byte field. That is a theoretical 256 distinct in-flight tags, but actual concurrency is bounded by device-declared queue depth, not the tag field width.
- **New formalization in 4.1**: devices declare one of two queueing architectures via descriptors, **shared queue** (`bQueueDepth` in Device Descriptor, one depth across all LUs) or **per-LU queues** (`bLUQueueDepth` per Unit Descriptor). If both are 0, queue depth is "unknown" (backward-compat fallback). RPMB LU can use the shared queue *or* have its own dedicated fixed-depth queue independent of how other LUs are configured.
- This device-level queue-depth declaration is separate from, and layered on top of, the HCI-level (UTRD/MCQ) slot mechanics below. A UFS device can declare a *logical* queue depth smaller than the HCI's physical slot count.

### 2.3 Legacy single-queue mode (JESD223C-equivalent baseline, confirmed still present in JESD223F)

- Up to **32 UTRD slots** (0's-based NUTRS field, min 1 / max 32).
- Each **UTRD is 8 DWORDs (32 bytes)**: DW0 command type/flags/crypto index, DW1/DW3 crypto data-unit-number, DW2 Overall Command Status (OCS), DW4/DW5 UTP Command Descriptor (UCD) base address, DW6 Response UPIU offset/length, DW7 PRDT offset/length.
- **One 32-bit doorbell bitmap register, UTRLDBR at offset 58h.** One bit per slot, host sets a bit to submit, controller clears it on completion.
- **Concrete, previously unknown arbitration rule found in the spec**: *"the host controller always processes transfer requests in-order according to the order submitted to the list... a transfer request with a lower index value will be executed before a transfer request with a higher index value."* Execution order is by **slot index, not by the order doorbells were rung.** This matters for the simulator's arbitration model and was not something the original NVMe-derived assumption would have gotten right by default.
- **PRDT (scatter/gather) entry: 4 DWORDs (16 bytes)**, 18-bit 0's-based byte count field, max 262,144 bytes (256 KB) per segment.

### 2.4 Multi-Circular Queue (MCQ), the major gap the original plan couldn't resolve, now closed via JESD223F

MCQ and legacy single-doorbell mode are **mutually exclusive per controller instance**, selected at init via `MCQConfig` register (offset 380h) bit 0 (QT: 0=legacy, 1=MCQ), not a simultaneous or coexisting pair of modes. This is a genuine hardware fact, not just a simulator simplification, and it directly informs the open decision in §7.

- **Queue count**: `MCQCAP.MAXQ` field, 0's-based, max value 31, so **up to 32 queues**. The spec requires the controller to implement up to MAXQ *completion* queues as well, to support 1:1 topology, but explicitly supports **N:1 SQ-to-CQ mapping**: each SQ carries a `CQID` field naming which CQ receives its completions. This is closer to NVMe's flexibility than the original plan assumed, and the queue-count ceiling (32) is 4x MQSim's current NVMe max (8).
- **Per-queue depth**: host-configurable via `SQATTRy.SIZE` / `CQATTRy.SIZE`, encoded as `(slot_count × 8) − 1` DWords, 16 bits wide, so **max 8192 slots per queue**, independently sized per SQ and CQ. No fixed protocol-level depth like legacy's 32-slot cap. A separate **global** cap, `MCQConfig.MAC` ("Max Active Command", default 0x1F = 32), limits total outstanding commands across all queues regardless of per-queue sizing.
- **SQ entry format: reused, not new.** "UTRD is pushed... into SQ" (JESD223F §6.2.1). Same 32-byte UTRD as legacy mode, only the container (host-memory circular queue versus fixed slot array plus bitmap) changes.
- **CQ entry format: new, MCQ-specific, but also 8 DWORDs/32 bytes.** Self-contained completion record carrying UCD base address, SQID (origin queue), Response UPIU offset/length, PRDT offset/length, Overall Status (OCS, note that OCS is in the CQ entry in MCQ mode, not in `UTRD.OCS` as in legacy mode), and LUN/Task Tag/IID fields, all copied from the originating SQ entry.
- **Doorbells: per-queue head/tail pointer pairs, but not fixed-stride/slot-index addressed like NVMe.** Each SQ/CQ has an indirection register (`SQDAOy`/`CQDAOy`) pointing to where its actual head/tail pointer registers are. The spec explicitly leaves exact MMIO placement implementation-defined. Pointers are **DWord offsets relative to the queue's base address**, not slot indices, 18 bits wide, DWord-aligned. Empty/full convention: head==tail means empty, and the design always reserves one wasted slot (stated explicitly, unlike NVMe where it's implicit).
- **Arbitration: simpler than NVMe, not more complex.** One global scheme selector (`MCQConfig.AS`): round-robin (0) or strict priority (1). **No weighted-round-robin or priority-quantum mechanism** like NVMe's URGENT/HIGH/MEDIUM/LOW classes. Per-queue priority in strict mode is a flat 8-level field (`SQATTRy.SQPL`, 0=highest). This is a genuine simplification versus what the original plan (extrapolating from NVMe) assumed Day 10 would need.
- **Interrupts**: two topologies, traditional per-queue IS/IE registers, or Event Specific Interrupt (ESI, MSI-X-like vector writes to a host-provided address, "recommended for MCQ"). **Interrupt aggregation/coalescing** is directly modelable: count-based (`IACTH`, up to 31 completions) and/or timeout-based (`IATOVAL`), grouped via up to 32 Interrupt Aggregation Groups. This is mechanically parallel to NVMe's interrupt coalescing, so the existing NVMe completion-batching logic is a legitimate reference to adapt rather than build from scratch.
- **No MCQ-specific timing/overhead numbers are stated anywhere in the spec** (queue creation cost, doorbell-write latency). Don't invent one. If the simulator needs a constant here, that's necessarily an assumption to flag, not a spec value.

### 2.5 Explicitly out of scope, reconfirmed with the real spec text

Task Management Functions, full power-mode state machine, Query Request *content* (attribute/flag/descriptor tables). All confirmed present in JESD220G/H exactly as expected, no surprises, still correctly excluded from a performance-fidelity model.

## 3. UFS 4.1 deltas, now cross-checked with the JESD220G Annex E.1 (the spec's own changelog) instead of web search

Verified against JESD220G's own "Changes Between JESD220G and JESD220F" annex (pp. 509-510), which the original plan did not have access to:

| Feature | What it actually is | Timing-model relevance |
|---|---|---|
| **MCQ** | See §2.4. Up to 32 SQ/CQ pairs, N:1 mapping, 8192-slot depth, simple RR/strict-priority arbitration, interrupt aggregation | **Core**, drives most of Phase C |
| Queue Depth Definition (shared vs. per-LU) | Formalizes `bQueueDepth`/`bLUQueueDepth` (§2.2) | Minor, a config knob, not new mechanics |
| WriteBooster Buffer Resize | Host-initiated resize via Query attributes | **Confirmed no interleaving impact**: spec states *"the device may adjust the WriteBooster Buffer size when the command queue is empty"*. It's a pure idle/background operation, never adds latency to an in-flight command. Simplifies what was originally flagged as an open timing question. |
| WriteBooster Partial Flush (FIFO / Pinned modes) | Attribute-negotiated write classification, not new queue mechanics | Capability-level only, no new timing model needed |
| Host Initiated Defragmentation (HID) | State machine (`bHIDState`/`bDefragOperation`) | **Same idle-gating pattern as WB resize.** Spec states the analysis/defrag phase won't start until the queue is empty, and any subsequent write-class command aborts it. Model as background-only, no in-flight interleaving. |
| RPMB Authenticated Vendor Specific Command | New RPMB request-message types (0010h/0011h) tunneled through existing SECURITY PROTOCOL IN/OUT SCSI commands | **New concrete latency-relevant fact**: this requires **at least 2 command round trips** (submit plus MAC validation, then a separate status-read round trip) versus 1 for ordinary read/write. Also, each RPMB region processes only 1 authenticated operation at a time (a serialization constraint independent of queue depth). Worth a small, explicit latency multiplier in the model. This wasn't captured with this precision in the original web-search-based plan. |
| Fast Recovery Mode | Device signals `FAST_RECOVERY_NEEDED` via existing Device Information field (no new UPIU) | Qualitative only. **Spec gives no numeric timeout/latency value.** If modeled at all, any timing constant here is an assumption, not a spec value, low priority, arguably skip. |
| Device Level Exception Event / Health Exception | New bits on the existing Exception Event mechanism | No new queueing mechanism, free (already-modeled) response-field bits, one-line note only |
| HS-Gear 5 | See §4 for exact numbers | **Core**, drives Phase C Day 11 |

## 4. UFS 5.0 deltas, cross-checked with the JESD220H Annex E.1, with one important correction to the original plan

JESD220H's own changelog (pp. 412-413) and direct inspection surfaced two corrections to the original (web-search-derived) delta list:

- **"Inline hashing" is not a real UFS 5.0 feature.** Exhaustive grep of JESD220H found zero mentions in any storage-relevant context. This was a web-search artifact from the original plan and should be dropped from scope entirely.
- **Link equalization is not new to 5.0.** The underlying Adapt/equalizer-training mechanism dates to an earlier revision (tied to HS-GEAR4/M-PHY 4.1). What's actually new in 5.0 is **persistent storage** of trained EQ settings (`qTxEQGnSettings`, `wTxEQGnSettingsExt`) so a device can reuse them across link startups, but the spec does not state whether this measurably shortens link bring-up time (explicitly called "implementation specific"). Treat as no documented timing impact, but don't conclude as "zero impact". It's spec-ambiguous, not spec-confirmed-negligible.

Confirmed real deltas, in priority order for this simulator:

| Feature | Detail | Timing-model relevance |
|---|---|---|
| **HS-Gear 6** | See the numbers below. Exactly 2x HS-Gear 5 per lane, same lane count (up to 2) | **Core**, drives Phase D |
| Isolated VCCQ_M power rail | New pin (confirmed absent from JESD220G text), purely electrical | Out of scope, no further action |
| Pre-Erase | Host-initiated advance-erase hint (WriteBooster + SLC only). Spec: *"there may be additional latency (usually ms-level) while waiting for the erase [without pre-erase]... Pre-Erase... optimiz[es] latency"* | **New, genuinely timing-relevant if WriteBooster/SLC modeling is in scope.** A legitimate write-latency-tail knob. Optional: only worth adding if the simulator already models WriteBooster SLC writes, otherwise note and skip. |
| Descriptor/Flag/Attribute Aggregation (new Query opcode 09h, "AGGREGATED READ") | Retrieves multiple descriptors/attributes/flags in one Query round trip | Reduces serialized Query round trips at device-enumeration/init time. Worth a one-line note in an init-sequence timing model even though query *content* stays out of scope, low priority |
| Zoned Storage for UFS (ZUFS) | Strict in-order LBA writes per zone, 6 or more zones open/closed concurrently required | Real queue-arbitration constraint (per-zone write serialization) **if** zoned-namespace modeling is ever added. Not currently in scope, flagged as a future extension point, not part of this plan |
| Third-party copy (EXTENDED COPY etc.) | SCSI-level optional opcodes, no new UPIU/CDB structural elements | Out of scope, note existence only |
| MCQ / doorbell / UTRD layer | **Unconfirmed for 5.0.** See §8, gap #1 | Working assumption: unchanged from UFSHCI 4.1 (JESD223F), since JESD220H's own device/transport layer shows no queue-depth-mechanism changes and the spec family has a stated pattern of HCI-layer stability across device-spec revisions. But this is an assumption, not a spec-confirmed fact, because no UFSHCI 5.0 document was available to check directly |

### HS-Gear bandwidth table (JESD220G Table 6.7, confirmed identical numbering scheme in JESD220H)

| HS-GEAR | Rate A-series | Rate B-series (fref 19.2/38.4 MHz) | Rate B-series (fref 26.0/52.0 MHz) |
|---|---|---|---|
| HS-GEAR4 | 9984 Mbps | 11673.6 Mbps | 11648.0 Mbps |
| HS-GEAR5 | 19968 Mbps | 23347.2 Mbps | 23296.0 Mbps |
| **HS-GEAR6** | **39936 Mbps** | **46694.4 Mbps** | **46592.0 Mbps** |

- Per-lane, per-direction, raw PHY rate. Up to **2 lanes** each direction (unchanged across 4.1 to 5.0), so max aggregate is about 2x the per-lane figure.
- **HS-Gear6 device support is explicitly mandatory** in JESD220H §8.4 ("a UFS device shall support... HS-GEAR6"). No equivalent explicit host-mandate sentence was found. Flagged as worth double-checking if host-side gear negotiation is modeled.
- **No line-coding/protocol efficiency percentage is stated in either spec** (only a generic "8b10b line coding per M-PHY" mention, no gear-specific number). If the simulator needs an effective-throughput-versus-raw-PHY-rate constant, that must be sourced from the MIPI M-PHY spec (not available yet) or treated as an explicit, documented assumption. Do not silently bake in an 80%-efficiency guess.

## 5. Concrete Gap Analysis (updated, cross-checked against the spec)

| Area | Files | Change |
|---|---|---|
| **Protocol enum & constants** | `src/ssd/Host_Interface_Defs.h` | Add `UFS` to `HostInterface_Types` enum. Add UFS register constants in their own address block. **Not** aliased onto NVMe's, since UFS's doorbell addressing (indirect `SQDAOy`/`CQDAOy` pointers, DWord-offset semantics) is structurally different from NVMe's fixed-stride slot-index doorbells (§2.4). |
| **New host interface** | new: `src/ssd/Host_Interface_UFS.h/.cpp` | Subclass `Host_Interface_Base`. Command descriptor is the 32-byte UTRD (§2.3), reused unchanged for MCQ's SQ entries (§2.4). One struct serves both legacy and MCQ mode, only the doorbell/queue container differs. New 32-byte CQ-entry struct needed for MCQ mode only (§2.4). Legacy mode has no equivalent, it just clears a UTRLDBR bit. |
| **Stream/fetch collaborators** | new: `Input_Stream_UFS`, `Request_Fetch_Unit_UFS` | Reuse `segment_user_request`. Protocol-neutral, needs no new logic beyond connecting the pieces. |
| **Transport dispatch** | `src/host/PCIe_Root_Complex.cpp` (~L9-71) | Add a UFS branch to the `SSD_device_type` switch. Given MCQ's queue-count ceiling (32, versus NVMe's 8) and its own arbitration scheme (plain round-robin or flat 8-level strict priority, simpler than NVMe's weighted URGENT/HIGH/MEDIUM/LOW, §2.4), a UFS-specific arbitration path is warranted rather than trying to reuse NVMe's weighted-round-robin code as-is. |
| **Config integration (5+ sites)** | `src/exec/Device_Parameter_Set.cpp/.h`, `src/exec/SSD_Device.cpp` (~L200-262, 351-365), `src/exec/Host_System.cpp` (L17-21, 36-43, 83-86, 94-96, 116-130), `src/utils/Logical_Address_Partitioning_Unit.cpp` | Every site does an exhaustive switch/if over the closed 2-value enum. Each needs a third case. Several fall through silently on an unmatched case instead of reporting error, so a missed site fails quietly rather than loudly. Check each one manually. |
| **Build system** | `MQSim.vcxproj` / `MQSim.vcxproj.filters` | Mechanical copy-paste of the NVMe/SATA `<ClCompile>`/`<ClInclude>` entries. Makefile globs automatically, no manual registration needed there. |
| **Cleanup** (noted while analysing this code) | `src/exec/Host_System.cpp` L17, L83 | Two places cast `ssd_host_interface` to `Host_Interface_NVMe*` purely to call the shared base's `GetType()`. Fragile with a third subtype in play. |

## 6. Day-by-Day Plan (~18 working days), revised

### Phase A: Design (Days 1-3)

**Day 1: Register and descriptor design**
Finalize the UTRD layout (now spec-confirmed: 8 DWORDs/32 bytes, §2.3) and the legacy doorbell scheme (single 32-bit UTRLDBR bitmap, slot-index execution order, §2.3, a concrete rule the original plan didn't have). Decide baseline queue depth: 32 slots for legacy mode. Sketch the MCQ CQ-entry struct (§2.4) even though it isn't implemented until Phase C, so the UTRD/CQ-entry pair is designed together.

**Day 2: Class skeletons**
Write header-only skeletons for `Host_Interface_UFS`, `Input_Stream_UFS`, `Request_Fetch_Unit_UFS`, and the transport HBA, following the NVMe/SATA pattern. Stub all methods. Confirm it compiles alongside the existing NVMe/SATA code.

**Day 3: Enum adaptation and regression baseline**
Add `UFS` to `HostInterface_Types` and touch all 5+ switch sites with placeholder cases. Run the existing NVMe/SATA configs to confirm no regression before any real UFS logic is added.

### Phase B: Single-queue UTP baseline (Days 4-8)

**Day 4: Command descriptor and doorbell simulation**
Implement the UTRD struct and UTRLDBR bitmap doorbell-ring simulation in `Request_Fetch_Unit_UFS`. Implement the slot-index-order execution rule (§2.3). This is the one legacy-mode behavior most likely to be gotten wrong by copying NVMe's doorbell-order assumption.

**Day 5: Command fetch to User_Request**
Implement command fetch and parsing into `User_Request` (LBA/length extraction only). Account for UPIU header overhead as fixed added latency: 32-byte COMMAND UPIU, 24-byte DATA IN/OUT header (§2.1), now exact spec numbers, not estimates.

**Day 6: Completion path**
Connect the completion path: UTRD write-back (OCS field), doorbell-clear, interrupt. Reuse `segment_user_request` and the existing TSU/AMU pipeline unchanged.

**Day 7: Config and transport integration**
Finish XML config parsing (`Host_Interface_Type=UFS`) and the real switch-case logic at all sites identified in Phase A/Day 3. Connect `PCIe_Root_Complex` dispatch.

**Day 8: First end-to-end run**
Add build-system entries (vcxproj). Run a first smoke test with a synthetic single-queue workload. Sanity-compare timing behavior against the existing SATA single-queue baseline.

### Phase C: MCQ / UFS 4.1 (Days 9-13), expanded from 4 to 5 days given the added concrete scope in §2.4

**Day 9: MCQ queue structures**
Implement the SQ (reused UTRD) and new CQ-entry (§2.4) structs, plus the `SQATTRy`/`CQATTRy`-style per-queue config (size, base address, CQID mapping for N:1 support). Support up to 32 SQ/CQ pairs, 4x NVMe's current 8-pair ceiling, so check any place MQSim hardcodes an 8-queue assumption.

**Day 10: Doorbell indirection and arbitration**
Implement per-queue head/tail pointer doorbells with DWord-offset semantics (not slot-index, unlike legacy mode, §2.4). Implement arbitration: plain round-robin or flat 8-level strict priority (`SQPL`), simpler than NVMe's weighted-round-robin/priority-class scheme, so this day should come in under the original estimate.

**Day 11: Interrupt aggregation**
Implement count-based (`IACTH`) and timeout-based (`IATOVAL`) completion-interrupt coalescing, grouped via Interrupt Aggregation Groups. This is new scope versus the original plan (not previously identified) but mechanically close enough to NVMe's existing interrupt-coalescing logic to adapt rather than build fresh.

**Day 12: HS-Gear 5 timing calibration and RPMB round-trip modeling**
Add HS-Gear 5 bandwidth constants (19968/23347.2/23296.0 Mbps per lane, §4 table, exact spec numbers now, not estimates). Add the RPMB authenticated-command latency multiplier (2 or more round trips instead of 1, §3) if RPMB traffic is exercised by the target workloads, otherwise stub and document as a known simplification.

**Day 13: Validation**
Run the existing `fast18` trace workloads against the UFS 4.1 config. Compare queue-depth scaling behavior against NVMe and SATA baselines (note: UFS's queue ceiling is 4x NVMe's, so scaling tests should push past 8 queues to actually exercise MCQ's extra range). Fix bugs surfaced by the comparison.

### Phase D: UFS 5.0 delta (Days 14-16)

**Day 14: HS-Gear 6 constants**
Add HS-Gear 6 bandwidth constants (39936/46694.4/46592.0 Mbps per lane, exactly 2x Gear 5, §4 table) as a version/gear config parameter on the same 4.1 code path.

**Day 15: Scope confirmation**
Confirm the working assumption that MCQ/doorbell/UTRD are unchanged from UFSHCI 4.1 for 5.0 (§4, §8 gap #1, genuinely unconfirmed, no UFSHCI 5.0 doc was available). Drop "inline hashing" from scope entirely (§4, confirmed not a real feature). Document link equalization's persistent-EQ-storage as no-timing-impact-but-spec-ambiguous, and Pre-Erase as optional/skip-unless-WriteBooster-SLC-is-modeled (§4).

**Day 16: Validation**
Run a comparison across UFS 4.1 and 5.0 configs. Confirm the bandwidth increase shows up correctly in throughput and latency statistics.

### Phase E: Testing, docs, wrap-up (Days 17-18, +2 day buffer)

**Day 17: Example configs**
Write example XML configs: `ssdconfig-ufs-4.1.xml`, `ssdconfig-ufs-5.0.xml`, following the existing `fast18` config conventions.

**Day 18: Full regression pass and documentation**
Confirm existing NVMe/SATA configs produce byte-for-byte identical output to pre-change runs. Short README section describing the UFS model, its parameters, and its explicit scope boundaries (§2.5, §8). Address remaining risk items: the fragile `NVMe*` casts, any silent-fallthrough switches found along the way.

**Days 19-20: Buffer**
Reserved for fixes surfaced during validation and any scope adjustments, including, sourcing the actual UFSHCI 5.0 spec to close gap #1 in §8 rather than sticking to the "unchanged from 4.1" assumption.

## 7. Open Decision Before Phase B (updated framing, not resolved)

Confirm before Phase B starts: build the single-queue legacy UTP baseline first (Phase B) as a stepping stone, or skip straight to MCQ (Phase C)?

**New context from this analysis that bears on the decision**: legacy single-doorbell mode and MCQ are **mutually exclusive, hardware-selectable modes on real UFS 4.x/5.x controllers** (`MCQConfig.QT` bit, §2.4), not a deprecated-versus-current split. A real UFS 4.1 host is expected to support both and choose one at init. So building legacy mode first is not wasted effort. It's a real, spec-sanctioned operating mode that happens to also make a good stepping-stone checkpoint (simpler doorbell/arbitration model, earlier smoke-testable). The plan above still assumes building it first (Phase B before Phase C), on those grounds, but this remains open to confirm, not something resolved unilaterally here.

## 8. Known gaps / assumptions, read before treating any MCQ-for-5.0 claim as fact

1. **UFSHCI 5.0 spec was not available for this revision.** JESD223F (UFSHCI 4.1) is the newest HCI-layer document on hand. Everything in §2.4 (MCQ architecture) is cross-checked with the JESD223F and is solid for the **4.1 target**. For the **5.0 target**, §4's claim that MCQ/doorbell/UTRD carry over unchanged is a **working assumption**, not a spec-confirmed fact. Source the real UFSHCI 5.0 document (likely JESD223G or similar) before finalizing Phase D if precision matters here.
2. **No line-coding/protocol-efficiency percentage is stated in JESD220G or JESD220H** for any HS-Gear. The bandwidth table in §4 gives raw PHY Mbps only. Any "effective throughput" constant used in the simulator is necessarily an assumption pending the MIPI M-PHY spec (not available).
3. **Fast Recovery Mode has no numeric timeout in the spec** (§3). Qualitative only. Skip modeling it, or clearly label any constant used as invented.
4. **MCQ-specific timing/overhead constants (queue creation cost, doorbell-write latency) are not stated anywhere in JESD223F** (§2.4). If the simulator needs one, it's an assumption.
5. **Host-side HS-Gear6 support mandate is not explicitly stated** in JESD220H (only the device-side mandate is). Worth double-checking if host-side gear negotiation logic is modeled (§4).
