# Adding UFS 4.1 / UFS 5.0 Support to MQSim

**Architecture gap analysis and a day-by-day implementation plan**

- Repository: MQSim (SSD simulator)
- Baseline reference (original plan): JEDEC JESD220 (UFS 1.0/1.1 base architecture, 2011), plus web-search summaries of UFS 4.1/5.0 deltas
- **Reference (2026-07-16 revision): the actual current specs.** JESD220G (UFS 4.1 device/transport spec, Dec 2024), JESD220H (UFS 5.0 device/transport spec), and JESD223F (UFSHCI 4.1 host-controller-interface spec, Jan 2025, the document that actually defines MCQ/doorbell/UTRD. This was **not** covered by JESD220 at all and had to be sourced separately).
- **Reference (2026-07-17 revision): JESD223G (UFSHCI 5.0)** added, closing the one remaining gap from the previous revision. See §4 and §5.
- Prepared: 2026-07-15. Revised: 2026-07-16 (real 4.1/5.0 specs). **Revised: 2026-07-17** (UFSHCI 5.0 spec, resolved the legacy-versus-MCQ open decision).

## Scope decisions

- **Fidelity target: performance-simulation fidelity.** Model UFS's timing-relevant behavior (queue/doorbell semantics, UPIU header overhead, HS-Gear bandwidth/latency) at the same abstraction level MQSim already uses for NVMe/SATA. Full SCSI CDB parsing, task management functions, query request *content*, and the power-mode state machine are explicitly out of scope.
- **Versioning: sequential milestones.** A working UFS 4.1 baseline (MCQ) first, then UFS 5.0 as a smaller follow-on delta (HS-Gear 6 bandwidth constants) on the same code path.
- **Queue architecture: build MCQ directly, no legacy single-doorbell stepping stone.** *(Resolved 2026-07-17, see §5 for the reasoning and what this replaces.)* The simulator goes straight to Multi-Circular Queue mode for the UFS 4.1 target. Legacy single-doorbell mode is documented (§2.3) for context but not implemented.
- **Timeline: ~15 working days** (+2 day buffer), reduced from the original ~18-day estimate now that legacy-mode scaffolding is no longer being built. See §7.

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

### 2.3 Legacy single-queue mode, documented for reference, **not implemented** (see §5)

Kept here because MCQ's SQ entry format directly reuses this mode's UTRD structure, worth understanding even though the legacy doorbell/bitmap mechanics themselves aren't being built.

- Up to **32 UTRD slots** (0's-based NUTRS field, min 1 / max 32).
- Each **UTRD is 8 DWORDs (32 bytes)**: DW0 command type/flags/crypto index, DW1/DW3 crypto data-unit-number, DW2 Overall Command Status (OCS), DW4/DW5 UTP Command Descriptor (UCD) base address, DW6 Response UPIU offset/length, DW7 PRDT offset/length.
- **One 32-bit doorbell bitmap register, UTRLDBR at offset 58h.** One bit per slot, host sets a bit to submit, controller clears it on completion.
- Execution order is by **slot index, not by the order doorbells were rung.** An explicit spec statement, not an NVMe-derived assumption.
- **PRDT (scatter/gather) entry: 4 DWORDs (16 bytes)**, 18-bit 0's-based byte count field, max 262,144 bytes (256 KB) per segment. This PRDT format is shared with MCQ mode (§2.4). It does not change between the two.

### 2.4 Multi-Circular Queue (MCQ), the architecture this plan actually builds

MCQ and legacy single-doorbell mode are **mutually exclusive per controller instance**, selected at init via `MCQConfig` register (offset 380h) bit 0 (QT: 0=legacy, 1=MCQ), not a simultaneous or coexisting pair of modes. This is a genuine hardware fact: a real UFS 4.1/5.0 controller picks one mode and stays in it. MCQ is what this plan implements directly (§5).

- **Queue count**: `MCQCAP.MAXQ` field, 0's-based, max value 31, so **up to 32 queues**. The spec requires the controller to implement up to MAXQ *completion* queues as well, to support 1:1 topology, but explicitly supports **N:1 SQ-to-CQ mapping**: each SQ carries a `CQID` field naming which CQ receives its completions. The queue-count ceiling (32) is 4x MQSim's current NVMe max (8).
- **Per-queue depth**: host-configurable via `SQATTRy.SIZE` / `CQATTRy.SIZE`, encoded as `(slot_count × 8) − 1` DWords, 16 bits wide, so **max 8192 slots per queue**, independently sized per SQ and CQ. A separate **global** cap, `MCQConfig.MAC` ("Max Active Command", default 0x1F = 32), limits total outstanding commands across all queues regardless of per-queue sizing.
- **SQ entry format: reused, not new.** "UTRD is pushed... into SQ" (JESD223F §6.2.1). Same 32-byte UTRD as legacy mode (§2.3), only the container (host-memory circular queue versus fixed slot array plus bitmap) changes.
- **CQ entry format: new, MCQ-specific, but also 8 DWORDs/32 bytes.** Self-contained completion record carrying UCD base address, SQID (origin queue), Response UPIU offset/length, PRDT offset/length, Overall Status (OCS, note that OCS is in the CQ entry in MCQ mode, not in `UTRD.OCS` as in legacy mode), and LUN/Task Tag/IID fields, all copied from the originating SQ entry.
- **Doorbells: per-queue head/tail pointer pairs, but not fixed-stride/slot-index addressed like NVMe.** Each SQ/CQ has an indirection register (`SQDAOy`/`CQDAOy`) pointing to where its actual head/tail pointer registers are. The spec explicitly leaves exact MMIO placement implementation-defined. Pointers are **DWord offsets relative to the queue's base address**, not slot indices, 18 bits wide, DWord-aligned. Empty/full convention: head==tail means empty, and the design always reserves one wasted slot (stated explicitly, unlike NVMe where it's implicit).
- **Arbitration: simpler than NVMe, not more complex.** One global scheme selector (`MCQConfig.AS`): round-robin (0) or strict priority (1). **No weighted-round-robin or priority-quantum mechanism** like NVMe's URGENT/HIGH/MEDIUM/LOW classes. Per-queue priority in strict mode is a flat 8-level field (`SQATTRy.SQPL`, 0=highest).
- **Interrupts**: two topologies, traditional per-queue IS/IE registers, or Event Specific Interrupt (ESI, MSI-X-like vector writes to a host-provided address, "recommended for MCQ"). **Interrupt aggregation/coalescing** is directly modelable: count-based (`IACTH`, up to 31 completions) and/or timeout-based (`IATOVAL`), grouped via up to 32 Interrupt Aggregation Groups. This is mechanically parallel to NVMe's interrupt coalescing, so the existing NVMe completion-batching logic is a legitimate reference to adapt.
- **No MCQ-specific timing/overhead numbers are stated anywhere in the spec** (queue creation cost, doorbell-write latency). Don't invent one. If the simulator needs a constant here, that's necessarily an assumption to flag, not a spec value.
- **Confirmed unchanged in UFSHCI 5.0 (JESD223G)**, every bullet above, verbatim, per JESD223G's own Annex A.1 changelog and independent cross-check of the register tables (§4). MCQ is the same architecture for both the 4.1 and 5.0 targets. No version-conditional MCQ logic is needed anywhere in the implementation.

### 2.5 Explicitly out of scope, reconfirmed with the real spec text

Task Management Functions, full power-mode state machine, Query Request *content* (attribute/flag/descriptor tables). All confirmed present in JESD220G/H exactly as expected, no surprises, still correctly excluded from a performance-fidelity model.

## 3. UFS 4.1 deltas, now cross-checked with the JESD220G Annex E.1 (the spec's own changelog) instead of web search

Verified against JESD220G's own "Changes Between JESD220G and JESD220F" annex (pp. 509-510), which the original plan did not have access to:

| Feature | What it actually is | Timing-model relevance |
|---|---|---|
| **MCQ** | See §2.4. Up to 32 SQ/CQ pairs, N:1 mapping, 8192-slot depth, simple RR/strict-priority arbitration, interrupt aggregation | **Core**, drives most of Phase B |
| Queue Depth Definition (shared vs. per-LU) | Formalizes `bQueueDepth`/`bLUQueueDepth` (§2.2) | Minor, a config knob, not new mechanics |
| WriteBooster Buffer Resize | Host-initiated resize via Query attributes | **Confirmed no interleaving impact**: spec states *"the device may adjust the WriteBooster Buffer size when the command queue is empty"*. It's a pure idle/background operation, never adds latency to an in-flight command. Simplifies what was originally flagged as an open timing question. |
| WriteBooster Partial Flush (FIFO / Pinned modes) | Attribute-negotiated write classification, not new queue mechanics | Capability-level only, no new timing model needed |
| Host Initiated Defragmentation (HID) | State machine (`bHIDState`/`bDefragOperation`) | **Same idle-gating pattern as WB resize.** Spec states the analysis/defrag phase won't start until the queue is empty, and any subsequent write-class command aborts it. Model as background-only, no in-flight interleaving. |
| RPMB Authenticated Vendor Specific Command | New RPMB request-message types (0010h/0011h) tunneled through existing SECURITY PROTOCOL IN/OUT SCSI commands | **New concrete latency-relevant fact**: this requires **at least 2 command round trips** (submit plus MAC validation, then a separate status-read round trip) versus 1 for ordinary read/write. Also, each RPMB region processes only 1 authenticated operation at a time (a serialization constraint independent of queue depth). Worth a small, explicit latency multiplier in the model. This wasn't captured with this precision in the original web-search-based plan. |
| Fast Recovery Mode | Device signals `FAST_RECOVERY_NEEDED` via existing Device Information field (no new UPIU) | Qualitative only. **Spec gives no numeric timeout/latency value.** If modeled at all, any timing constant here is an assumption, not a spec value, low priority, arguably skip. |
| Device Level Exception Event / Health Exception | New bits on the existing Exception Event mechanism | No new queueing mechanism, free (already-modeled) response-field bits, one-line note only |
| HS-Gear 5 | See §4 for exact numbers | **Core**, drives Phase B |

## 4. UFS 5.0 deltas, cross-checked with JESD220H and now JESD223G, gap closed

### 4.1 Device/transport layer (JESD220H Annex E.1)

Two corrections to the original web-search-derived delta list:

- **"Inline hashing" is not a device/transport-layer (JESD220H) feature.** Exhaustive grep of JESD220H found zero mentions in any storage-relevant context. *(Correction, 2026-07-17: it is a real UFS 5.0 feature, but it is entirely at the HCI/crypto layer, see §4.2. The 2026-07-16 revision calling it a "web-search artifact" was based only on the device spec and was incorrect. The HCI spec confirms it's real, just not where originally assumed.)*
- **Link equalization is not new to 5.0.** The underlying Adapt/equalizer-training mechanism dates to an earlier revision (tied to HS-GEAR4/M-PHY 4.1). What's new in 5.0 is **persistent storage** of trained EQ settings (`qTxEQGnSettings`, `wTxEQGnSettingsExt`). The spec does not state whether this measurably shortens link bring-up time (explicitly called "implementation specific"). No documented timing impact, but spec-ambiguous, not spec-confirmed-negligible.

| Feature | Detail | Timing-model relevance |
|---|---|---|
| **HS-Gear 6** | See the table below. Exactly 2x HS-Gear 5 per lane, same lane count (up to 2) | **Core**, drives Phase C |
| Isolated VCCQ_M power rail | New pin, purely electrical | Out of scope, no further action |
| Pre-Erase | Host-initiated advance-erase hint (WriteBooster + SLC only). Spec: *"there may be additional latency (usually ms-level) while waiting for the erase [without pre-erase]... Pre-Erase... optimiz[es] latency"* | Timing-relevant only if WriteBooster/SLC modeling is in scope. Optional, note and skip otherwise |
| Descriptor/Flag/Attribute Aggregation (Query opcode 09h) | Retrieves multiple descriptors/attributes/flags in one Query round trip | Reduces serialized Query round trips at init time, one-line note, low priority |
| Zoned Storage for UFS (ZUFS) | Strict in-order LBA writes per zone, 6 or more zones open/closed concurrently required | Real per-zone write-serialization constraint **if** zoned-namespace modeling is ever added. Not in scope, flagged as a future extension point |
| Third-party copy (EXTENDED COPY etc.) | SCSI-level optional opcodes | Out of scope, note existence only |

### 4.2 HCI layer (JESD223G vs. JESD223F), **gap closed 2026-07-17**

JESD223G's own Annex A.1 changelog ("Differences between JESD223G and JESD223F") is the complete list of HCI-layer changes for UFS 5.0. Independently cross-checked against every JESD223F register/field cited in §2.4, all confirmed byte-for-byte, bit-for-bit unchanged:

- **MCQ queue count (MAXQ=31 to 32 queues), per-queue depth (8192 slots), MCQConfig.MAC, SQ/CQ entry formats, SQDAOy/CQDAOy doorbell indirection, arbitration schemes (RR/strict-priority only), interrupt aggregation (IACTH/IATOVAL/MIAG), all confirmed unchanged from 4.1.** No version-conditional MCQ code path is needed (§2.4).
- Legacy single-doorbell mode is still present and documented in JESD223G (not deprecated), irrelevant to this plan since legacy mode isn't being built either way (§5).
- **Genuinely new in UFSHCI 5.0**: HS-Gear 6 negotiation happens at the MIPI UniPro 3.0 / M-PHY 6.0 attribute layer (new TX Equalization Training procedure). Two *existing* HCI status bits (`IS.UPMS`, `HCS.UPMCRS`) now also fire on TX-EQTR completion, in addition to power-mode-change completion. No new capability register gates "Gear 6 supported" at the HCI level.
- **New register**: `ESIVB` (Event Specific Interrupt Vector Base, offset 380h+0Ch), a minor addend applied to ESI vector writes. Trivial, no timing-model impact beyond what ESI already required.
- **"Inline hashing" is real, and is defined here**: new crypto algorithms SHA-256/SHA-512, plus updated 4DW/2DW PRDT formats to support them (JESD223G §5.8.2/§9.5). This is a security/crypto feature at the PRDT/data-path level. **Still out of scope** per the fidelity target. No timing-relevant number is associated with it in the spec, and it's an optional per-command feature, not a structural change to the PRDT format's timing-relevant fields (offset/length). It exists, it's just not worth modeling.
- **Zoned Storage (ZUFS)**: not addressed in JESD223G at all, no HCI-level queue/completion mechanics were added for it. Consistent with §4.1's "not in scope."

**Bottom line**: UFSHCI 5.0 is a functional no-op carry-over from 4.1 for every timing-relevant mechanism this plan models. Phase C (§7) needs no new MCQ/register-layer engineering, only the HS-Gear 6 bandwidth constants.

### HS-Gear bandwidth table (JESD220G Table 6.7, confirmed identical numbering scheme in JESD220H)

| HS-GEAR | Rate A-series | Rate B-series (fref 19.2/38.4 MHz) | Rate B-series (fref 26.0/52.0 MHz) |
|---|---|---|---|
| HS-GEAR4 | 9984 Mbps | 11673.6 Mbps | 11648.0 Mbps |
| HS-GEAR5 | 19968 Mbps | 23347.2 Mbps | 23296.0 Mbps |
| **HS-GEAR6** | **39936 Mbps** | **46694.4 Mbps** | **46592.0 Mbps** |

- Per-lane, per-direction, raw PHY rate. Up to **2 lanes** each direction (unchanged across 4.1 to 5.0), so max aggregate is about 2x the per-lane figure.
- **HS-Gear6 device support is explicitly mandatory** in JESD220H §8.4 ("a UFS device shall support... HS-GEAR6"). No equivalent explicit host-mandate sentence was found. Flagged as worth double-checking if host-side gear negotiation is modeled.
- **No line-coding/protocol efficiency percentage is stated in either spec** (only a generic "8b10b line coding per M-PHY" mention, no gear-specific number). If the simulator needs an effective-throughput-versus-raw-PHY-rate constant, that must be sourced from the MIPI M-PHY spec (not available yet) or treated as an explicit, documented assumption. Do not silently bake in an 80%-efficiency guess.

## 5. Legacy-versus-MCQ decision, resolved 2026-07-17

**Decision: implement MCQ directly for the UFS 4.1 target. Legacy single-doorbell mode is not built.**

This closes the open decision carried since the 2026-07-15 plan. Reasoning:

- The user's stated preference was to start directly with UFS 4.1 rather than an intermediate legacy stepping stone.
- Legacy and MCQ are mutually exclusive, hardware-selectable modes on a real controller (§2.4). Building legacy first was never strictly required for correctness, only useful as an earlier, simpler checkpoint. Skipping it means giving up that earlier checkpoint to save about 3 days of work (§7), since the legacy-mode UTRD/doorbell/completion-path engineering doesn't need to be built and then partially superseded.
- The SQ entry format is a straight reuse of the legacy UTRD struct (§2.3, §2.4) regardless of which mode is built, so no design knowledge from the legacy exploration is lost. It's documented in §2.3 for reference even though unimplemented.
- Net effect on the day-by-day plan (§7): the former "Phase B, Single-queue UTP baseline" is removed. Its still-needed pieces (command-descriptor struct, command fetch/parse, completion path, config/transport integration) are combined directly into the MCQ implementation phase instead of being built twice.

## 6. Concrete Gap Analysis

| Area | Files | Change |
|---|---|---|
| **Protocol enum & constants** | `src/ssd/Host_Interface_Defs.h` | Add `UFS` to `HostInterface_Types` enum. Add UFS register constants in their own address block. **Not** aliased onto NVMe's, since UFS's doorbell addressing (indirect `SQDAOy`/`CQDAOy` pointers, DWord-offset semantics) is structurally different from NVMe's fixed-stride slot-index doorbells (§2.4). |
| **New host interface** | new: `src/ssd/Host_Interface_UFS.h/.cpp` | Subclass `Host_Interface_Base`. Command descriptor is the 32-byte UTRD (§2.3), used directly as MCQ's SQ entry format (§2.4). No legacy mode is built, so this struct only ever operates inside a circular-queue container. New 32-byte CQ-entry struct needed for completions (§2.4), MCQ-specific, no legacy equivalent exists to reuse. |
| **Stream/fetch collaborators** | new: `Input_Stream_UFS`, `Request_Fetch_Unit_UFS` | Reuse `segment_user_request`. Protocol-neutral, needs no new logic beyond connecting the pieces. |
| **Transport dispatch** | `src/host/PCIe_Root_Complex.cpp` (~L9-71) | Add a UFS branch to the `SSD_device_type` switch. Given MCQ's queue-count ceiling (32, versus NVMe's 8) and its own arbitration scheme (plain round-robin or flat 8-level strict priority, simpler than NVMe's weighted URGENT/HIGH/MEDIUM/LOW, §2.4), a UFS-specific arbitration path is warranted rather than reusing NVMe's weighted-round-robin code as-is. |
| **Config integration (5+ sites)** | `src/exec/Device_Parameter_Set.cpp/.h`, `src/exec/SSD_Device.cpp` (~L200-262, 351-365), `src/exec/Host_System.cpp` (L17-21, 36-43, 83-86, 94-96, 116-130), `src/utils/Logical_Address_Partitioning_Unit.cpp` | Every site does an exhaustive switch/if over the closed 2-value enum. Each needs a third case. Several fall through silently on an unmatched case instead of reporting error, so a missed site fails quietly rather than loudly. Check each one manually. |
| **Build system** | `MQSim.vcxproj` / `MQSim.vcxproj.filters` | Mechanical copy-paste of the NVMe/SATA `<ClCompile>`/`<ClInclude>` entries. Makefile globs automatically, no manual registration needed there. |
| **Cleanup** (noted while analysing this code) | `src/exec/Host_System.cpp` L17, L83 | Two places cast `ssd_host_interface` to `Host_Interface_NVMe*` purely to call the shared base's `GetType()`. Fragile now that a third subtype exists. |

## 7. Day-by-Day Plan (~15 working days, +2 day buffer), restructured 2026-07-17 around MCQ-direct

### Phase A: Design (Days 1-3)

**Day 1: Register and descriptor design**
Design directly in MCQ terms (§2.4): the 32-byte UTRD reused as the SQ entry, the new 32-byte CQ-entry struct, `SQATTRy`/`CQATTRy`-style per-queue config fields (size, base address, `CQID` mapping for N:1 support), and the `SQDAOy`/`CQDAOy` doorbell-indirection addressing scheme. Decide queue count/depth ceilings for the simulator (spec allows up to 32 queues times 8192 slots, decide what subset is actually useful to expose as config knobs versus hardcoded).

**Day 2: Class skeletons**
Write header-only skeletons for `Host_Interface_UFS`, `Input_Stream_UFS`, `Request_Fetch_Unit_UFS`, and the transport HBA, following the NVMe/SATA pattern. Stub all methods. Confirm it compiles alongside the existing NVMe/SATA code.

**Day 3: Enum adaptation and regression baseline**
Add `UFS` to `HostInterface_Types` and touch all 5+ switch sites with placeholder cases. Run the existing NVMe/SATA configs to confirm no regression before any real UFS logic is added.

### Phase B: MCQ / UFS 4.1 (Days 4-11)

**Day 4: MCQ queue structures**
Implement the SQ (reused UTRD) and CQ-entry structs, plus per-queue config (size, base address, CQID mapping for N:1). Support up to 32 SQ/CQ pairs, 4x NVMe's current 8-pair ceiling, so check any place MQSim hardcodes an 8-queue assumption.

**Day 5: Doorbell indirection and arbitration**
Implement per-queue head/tail pointer doorbells with DWord-offset semantics via the `SQDAOy`/`CQDAOy` indirection scheme (§2.4). Implement arbitration: plain round-robin or flat 8-level strict priority (`SQPL`), simpler than NVMe's weighted-round-robin/priority-class scheme.

**Day 6: Command fetch to User_Request**
Implement command fetch and parsing into `User_Request` (LBA/length extraction only). Account for UPIU header overhead as fixed added latency: 32-byte COMMAND UPIU, 24-byte DATA IN/OUT header (§2.1), exact spec numbers, not estimates.

**Day 7: Completion path and interrupt aggregation**
Connect the completion path: CQ-entry write-back (OCS is in the CQ entry in MCQ mode, not a UTRD field, §2.4), doorbell/pointer update, interrupt. Implement count-based (`IACTH`) and timeout-based (`IATOVAL`) completion-interrupt coalescing, grouped via Interrupt Aggregation Groups, mechanically close enough to NVMe's existing interrupt-coalescing logic to adapt.

**Day 8: Config and transport integration**
Finish XML config parsing (`Host_Interface_Type=UFS`) and the real switch-case logic at all sites identified in Phase A/Day 3. Connect `PCIe_Root_Complex` dispatch.

**Day 9: HS-Gear 5 timing and RPMB round-trip modeling**
Add HS-Gear 5 bandwidth constants (19968/23347.2/23296.0 Mbps per lane, §4 table). Add the RPMB authenticated-command latency multiplier (2 or more round trips instead of 1, §3) if RPMB traffic is exercised by the target workloads, otherwise stub and document as a known simplification.

**Day 10: First end-to-end run**
Add build-system entries (vcxproj). Run a first smoke test with a synthetic multi-queue workload, there's no single-queue mode to fall back to, MCQ is the only path (§5). Sanity-compare timing behavior against NVMe's multi-queue baseline, the structurally closer reference now that legacy mode isn't in consideration.

**Day 11: Validation**
Run the existing `fast18` trace workloads against the UFS 4.1 config. Compare queue-depth scaling behavior against NVMe and SATA baselines, push past 8 queues to actually exercise MCQ's extra range (its ceiling is 4x NVMe's). Fix bugs surfaced by the comparison.

### Phase C: UFS 5.0 delta (Days 12-13)

**Day 12: HS-Gear 6 constants**
Add HS-Gear 6 bandwidth constants (39936/46694.4/46592.0 Mbps per lane, exactly 2x Gear 5, §4 table) as a version/gear config parameter on the same 4.1 code path. No MCQ/register-layer changes needed, confirmed no-op carry-over (§4.2), so this phase is bandwidth-constants-only.

**Day 13: Validation**
Run a comparison across UFS 4.1 and 5.0 configs. Confirm the bandwidth increase shows up correctly in throughput and latency statistics.

### Phase D: Testing, docs, wrap-up (Days 14-15, +2 day buffer)

**Day 14: Example configs**
Write example XML configs: `ssdconfig-ufs-4.1.xml`, `ssdconfig-ufs-5.0.xml`, following the existing `fast18` config conventions.

**Day 15: Full regression pass and documentation**
Confirm existing NVMe/SATA configs produce byte-for-byte identical output to pre-change runs. Short README section describing the UFS model, its parameters, and its explicit scope boundaries (§2.5, §8). Address remaining risk items: the fragile `NVMe*` casts, any silent-fallthrough switches found along the way.

**Days 16-17: Buffer**
Reserved for fixes surfaced during validation and any scope adjustments.

## 8. Known gaps / assumptions, read before treating any remaining claim as fact

1. ~~UFSHCI 5.0 spec not available~~, **closed 2026-07-17**, JESD223G confirms MCQ/doorbell/UTRD are unchanged from 4.1 (§4.2).
2. **No line-coding/protocol-efficiency percentage is stated in JESD220G, JESD220H, or JESD223G** for any HS-Gear. The bandwidth table in §4 gives raw PHY Mbps only. Any "effective throughput" constant used in the simulator is necessarily an assumption pending the MIPI M-PHY spec (not available yet).
3. **Fast Recovery Mode has no numeric timeout in the spec** (§3). Qualitative only. Skip modeling it, or clearly label any constant used as invented.
4. **MCQ-specific timing/overhead constants (queue creation cost, doorbell-write latency) are not stated anywhere in JESD223F or JESD223G** (§2.4). If the simulator needs one, it's an assumption.
5. **Host-side HS-Gear6 support mandate is not explicitly stated** in JESD220H (only the device-side mandate is). Worth double-checking if host-side gear negotiation is modeled (§4).
