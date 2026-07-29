# BeebSCSI `scsi.c` — compliance against the Adaptec ACB-4000 OEM Manual

**Reference:** *ACB-4000 5¼" Winchester Disk Controller OEM Manual*, Adaptec Inc., March 1984 (79 pp.). Section 6 "Command Specifications" and §6.4–6.5 "Completion Status / Sense Bytes".

**Subject:** `C:\Archlinux\Pi1MHz\src\BeebSCSI\scsi.c` (2465 lines), with supporting checks in `BeebSCSI/filesystem.c`.

**Method:** the manual's command set, CDB layouts, status/sense byte definitions and drive-parameter formats were extracted and compared field-by-field against every command handler in `scsi.c`.

---

## 0. The headline

`scsi.c` is **not, and does not aim to be, a strict ACB-4000 implementation.** It is an Acorn-host SCSI/SASI *emulation* that uses the ACB-4000 as its reference model (the ACB-4000 was the controller behind Acorn's SCSI interface), and the source comments quote the ACB-4000 manual throughout. What it actually implements is:

- **a subset** of the ACB-4000 command set (12 of 15 Class-0 commands, 2 of 6 Class-1 commands);
- **plus standard-SCSI commands the ACB-4000 never had** (INQUIRY 0x12, REASSIGN BLOCKS 0x07, READ DEFECT DATA(10) 0x37);
- **plus vendor extensions** (LV-DOS F-CODE group-6 commands, BeebSCSI SENSE/SELECT/FATPATH/FATINFO/FATREAD, CERTIFY).

Against that backdrop, the commands that *are* implemented mostly follow the ACB-4000 CDB and data formats correctly. The review originally found **2 clear bugs** (wrong sense-code constants), **1 off-by-one** (READ CAPACITY), and **~8 smaller deviations**. None of the vendor extensions can be judged against the manual — they are out of scope by definition.

**Update — fixes applied:** five of those items have since been corrected (deviations #1, #2, #3, #4, #9 in §6). Four remain open by choice (#5, #6, #7, #8 — deliberate ADFS-compatibility or regression-risk reasons; #6 was applied then reverted). See the §6 status table for the current state.

**Verdict:** *substantially compliant for the subset it implements* — and, after the fixes, fully compliant on sense codes, READ CAPACITY (last-block address and PMI validation) and START/STOP bit decoding.

---

## 1. Command-set coverage

### 1.1 Class 00 (6-byte commands) — manual Table 6-1

| Op | ACB-4000 command | In `scsi.c`? | Notes |
|----|------------------|-------------|-------|
| 00 | TEST UNIT READY | ✅ `scsiCommandTestUnitReady` (549) | compliant CDB; sense-code deviation — §4 |
| 01 | REZERO UNIT | ✅ `scsiCommandRezeroUnit` (583) | compliant |
| 03 | REQUEST SENSE | ✅ `scsiCommandRequestSense` (620) | compliant 4-byte sense |
| 04 | FORMAT UNIT | ✅ `scsiCommandFormat` (680) | minor deviations — §3 |
| 08 | READ | ✅ `scsiCommandRead6` (927) | compliant |
| 0A | WRITE | ✅ `scsiCommandWrite6` (1074) | compliant |
| 0B | SEEK | ✅ `scsiCommandSeek` (1206) | compliant |
| 0F | TRANSLATE | ✅ `scsiCommandTranslate` (1243) | compliant 8-byte response |
| 13 | WRITE BUFFER (1K diag) | ❌ **missing** | — |
| 14 | READ BUFFER (1K diag) | ❌ **missing** | — |
| 15 | MODE SELECT | ✅ `scsiCommandModeSelect6` (1399) | compliant 4+8+10 layout |
| 1A | MODE SENSE | ✅ `scsiCommandModeSense6` (1532) | byte-0 deviation — §3 |
| 1B | START/STOP UNIT | ✅ `scsiCommandStartStop` (1653) | bit-decode deviation — §3 |
| 1C | RECEIVE DIAGNOSTIC | ❌ **missing** | pairs with SEND DIAGNOSTIC |
| 1D | SEND DIAGNOSTIC | ✅ `scsiCommandSendDiagnostic` (1907) | stubbed — §3 |

Extra Class-0 op codes implemented that are **not** ACB-4000 commands:

| Op | Command | Handler | Status vs manual |
|----|---------|---------|------------------|
| 07 | REASSIGN BLOCKS | `scsiCommandReassignBlocks` (803) | standard SCSI, not in ACB-4000 — out of spec scope |
| 12 | INQUIRY | `scsiCommandInquiry` (1871) | SCSI-1/2 command, not in ACB-4000 — out of spec scope |

### 1.2 Class 01 (10-byte commands) — manual Table 6-3

| Op | ACB-4000 command | In `scsi.c`? | Notes |
|----|------------------|-------------|-------|
| 25 | READ CAPACITY | ✅ `scsiCommandReadCapacity` (1791) | off-by-one + missing PMI check — §3 |
| 28 | READ (extended) | ❌ **missing** | extended-LBA READ |
| 2A | WRITE (extended) | ❌ **missing** | extended-LBA WRITE |
| 2E | WRITE AND VERIFY | ❌ **missing** | — |
| 2F | VERIFY | ✅ `scsiCommandVerify` (1708) | ignores block count — §3 |
| 31 | SEARCH DATA EQUAL | ❌ **missing** | also: status bit 2 "Equal" therefore never used |

Extra Class-1 op code implemented that is **not** an ACB-4000 command:

| Op | Command | Handler | Status |
|----|---------|---------|--------|
| 37 | READ DEFECT DATA(10) | `scsiCommandReadDefectData10` (1837) | standard SCSI, not in ACB-4000 — out of scope |

### 1.3 Vendor / non-ACB groups

Group 6 (`0xC0`): LV-DOS WRITE/READ F-CODE; BeebSCSI SENSE/SELECT/FATPATH/FATINFO/FATREAD. Group 7 (`0xE0`): CERTIFY. **All outside the ACB-4000 manual — not assessable against the spec.** They are correctly placed in vendor-specific class codes (the manual §6.2.2 reserves class codes 2–7), so they do not collide with the standard set.

**Coverage summary:** of the 21 ACB-4000 commands (15 Class-0 + 6 Class-1), `scsi.c` implements **14**, omits **7** (WRITE BUFFER, READ BUFFER, RECEIVE DIAGNOSTIC, READ(10), WRITE(10), WRITE AND VERIFY, SEARCH DATA EQUAL).

---

## 2. CDB & general structure

| Manual rule (§6.2) | `scsi.c` | Compliant? |
|---|---|---|
| Byte 0 = class (bits 7-5) + opcode (bits 4-0) | `group = data[0]>>5`, `opCode = data[0]&0x1F` (392-393) | ✅ |
| Class 0 = 6-byte CDB, Class 1 = 10-byte | `switch(group){case 0:length=6; case 1:length=10;}` (397-401) | ✅ |
| LUN = byte 1, bits 7-5 (8 LUNs; ACB uses 0–1) | `targetLUN = (newLUN & 0x1E0)>>5` (439) — **4-bit** field | ⚠️ extends to 16 LUNs (deliberate BeebSCSI jukebox feature) |
| Class 0 LBA = 21-bit | `(data[1]&0x1F)<<16 | data[2]<<8 | data[3]` (969-971) | ✅ |
| Class 1 LBA = 32-bit | VERIFY: `data[2..5]` (1729-1733) | ✅ |
| Class 0 transfers ≤ 256 blocks; 0 = max | `numberOfBlocks=data[4]; if(0)=256` (974-975) | ✅ |
| Class 1 transfers ≤ 64K blocks; 0 = max | VERIFY debug: `data[7..8]; if(0)=65536` (1743) | ✅ |
| **Reserved bytes must be 0 or command is rejected** (§6.2.1) | not checked anywhere | ⚠️ **deviation** — see §3.1 |
| **Control byte: all bits reserved, must be 0** (§6.2.8) | not checked anywhere | ⚠️ **deviation** — see §3.1 |

---

## 3. Per-command deviations (commands that ARE implemented)

### 3.1 Reserved-byte / control-byte checking — whole file · LOW
The manual (§6.2.1, §6.2.8) states the ACB-4000 *rejects* any command whose reserved bytes or control byte are non-zero. `scsi.c` never inspects them. This makes BeebSCSI behave like the manual's **Appendix B "expanded option"** (lenient mode for non-SCSI-compliant hosts) rather than the default strict mode. For an Acorn host this is the desired behaviour, but it is a deliberate deviation from the default ACB-4000 contract. No fix recommended — just documenting it.

### 3.2 `scsiCommandFormat` (04) — fill pattern · LOW
- **Manual §6.3.1.4:** byte 2 is the fill character *only if* byte 1 bit 1 (unique-fill) is set; otherwise data fields are filled with `0x6C`.
- **`scsi.c:711`:** `dataPattern = commandDataBlock.data[2];` — byte 2 is taken unconditionally. A spec-conforming host that does not set the unique-fill bit leaves byte 2 = 0, so BeebSCSI would "fill" with `0x00`, not `0x6C`.
- In practice moot: `filesystemFormatLun` ignores `dataPattern` entirely (it uses a FAT fast-expand, see filesystem.c:845-846 "the dataPattern byte will be ignored"). So the format fill pattern is **not honoured at all** — a deviation from §6.3.1.4, though harmless for an emulation whose backing store is a host file.
- Also: the manual says FORMAT must be preceded by MODE SELECT or be rejected; `scsi.c` does not enforce that ordering. Minor.

### 3.3 `scsiCommandModeSense6` (1A) — parameter-header byte 0 · MEDIUM
- **Manual §6.3.1.11/§6.3.1.12:** MODE SENSE returns the *same* structure as MODE SELECT — a 4-byte parameter list whose **first three bytes are reserved (zero-filled)** and byte 3 = extent-descriptor length (8).
- **`scsi.c:1592-1607`:** byte 0 of the returned data is replaced with a computed value — `0` for page 0, or `length-1` (a SCSI-2 style "mode data length") for other pages — and the header send loop `for (i = 1; i < headerlen; i++)` deliberately **skips `headerptr[0]`**.
- So BeebSCSI emits a SCSI-2 MODE SENSE(6) header, not the ACB-4000 reserved-zero parameter list. Deviation in the returned data format. (It is internally consistent with whatever the BBC-side driver expects, but it does not match the manual.)

### 3.4 `scsiCommandStartStop` (1B) — START/STOP bit decode · MEDIUM · ✅ FIXED
- **Manual §6.3.1.13:** "Byte 04, **bit 00** … set if this is a START command, otherwise … STOP."
- **`scsi.c:1661`:** `if (commandDataBlock.data[4] == 0)` — tests the *whole byte*, not bit 0. A host that sends byte 4 = `0x02` (an even value, bit 0 clear → spec says STOP) is treated as START because the byte is non-zero.
- **Fix:** `if ((commandDataBlock.data[4] & 0x01) == 0)`.

### 3.5 `scsiCommandSendDiagnostic` (1D) — stub does not consume parameter data · MEDIUM
- **Manual §6.3.1.15:** SEND DIAGNOSTIC carries a parameter list (length in CDB bytes 3-4, ≥ 4 bytes; function codes 60H–65H). The data must be transferred in a DATA OUT phase.
- **`scsi.c:1907-1918`:** immediately returns `SCSI_STATUS_OK` without entering a DATA OUT phase. If a host issues SEND DIAGNOSTIC with a non-zero parameter length, those bytes are never read — the bus phase desynchronises. (Acceptable only because Acorn ADFS never sends a parameter list; still a deviation from §6.3.1.15.)
- The companion **RECEIVE DIAGNOSTIC (1C)** is not implemented at all, so the SEND→RECEIVE diagnostic dump sequence cannot work.

### 3.6 `scsiCommandReadCapacity` (25) — last-block address off-by-one · HIGH · ✅ FIXED
- **Manual §6.3.2.1:** READ CAPACITY "will return **the address of the last block** on the unit" — i.e. for an `N`-block drive, it must return `N − 1`.
- **`scsi.c:1807, 1814-1817`:** `lunsize = filesystemGetLunTotalSectors(...)` and that value is written directly as the 4-byte "last block address". But `filesystemGetLunTotalSectors` (filesystem.c:612-623) returns `Cylinders × Heads × SectorsPerTrack` — the **total count `N`**, not `N − 1`. The same function is used by VERIFY as an exclusive upper bound (`LBA >= lunSizeInSectors`, scsi.c:1752), confirming it is a count.
- **Result:** READ CAPACITY reports one block too many.
- **Fix:** send `lunsize - 1` (guarding `lunsize != 0`). *Caveat:* confirm against what Acorn ADFS expects before changing — if ADFS has always been fed the count, "fixing" it could shift behaviour; but per the manual this is a deviation.

### 3.7 `scsiCommandReadCapacity` (25) — PMI byte not validated · MEDIUM · ✅ FIXED
- **Manual §6.3.2.1:** CDB byte 8 (PMI) must be `00` or `01`; "Any value other than 00H or 01H in byte 08 will cause Check status with an error code of 24H." `01` should return the next cylinder-boundary block, not the last block.
- **`scsi.c`:** `commandDataBlock.data[8]` is never read. Any PMI value is silently treated as `00`. Deviation: missing the `0x24` (Bad Argument) rejection and the PMI=01 behaviour.

### 3.8 `scsiCommandVerify` (2F) — block count ignored · MEDIUM · ⚠️ NOT FIXED (reverted — see §6 note)
- **Manual §6.3.2.5:** VERIFY checks the ECC of *the specified number of blocks* starting at the LBA.
- **`scsi.c:1752`:** only `logicalBlockAddress >= lunSizeInSectors` is checked; the block count in CDB bytes 7-8 is read for debug only (1743) and never added. A VERIFY that *starts* in range but *spans past* the end of the LUN returns `SCSI_STATUS_OK`.
- **Fix:** compute `last = LBA + blockCount` and fail with `ILLEGAL_ADDR` when `last > lunSizeInSectors`. (This is also item §3.4 in the general code review.)

---

## 4. Sense codes & error handling

### 4.1 REQUEST SENSE data format — COMPLIANT
- Manual §6.4.1/§6.5: 4-byte sense; CDB byte 4 values 0–3 default to 4; byte 0 = [valid bit 7][error code bits 6-0]; bytes 1-3 = logical block address.
- `scsi.c:652-655`: writes 4 bytes — `[error code][LBA 0x1F mask][LBA][LBA]`. ✅ The 21-bit LBA mask (`0x1F0000`) matches the Class-0 address width. Always sends exactly 4 bytes regardless of CDB byte 4 — consistent with the manual ("default to 4 bytes", no >4 case). ✅

### 4.2 Error-code constants — `scsi.c:83-89`

| Macro | Value used | Manual code | Manual meaning | Verdict |
|-------|-----------|-------------|----------------|---------|
| `NO_ERROR` | `0x00` | 00 | No Sense | ✅ |
| `DRIVE_NOT_READY` | `0x04` | 04 | Drive Not Ready | ✅ |
| `BAD_ARG` | `0x24` | 24 | Bad Argument | ✅ |
| `ILLEGAL_ADDR` | `0xA1` | 21 | Illegal Block Address | ✅ `0xA1 = valid-bit(0x80) | 0x21` — correct (valid bit set because the LBA is reported) |
| `UNIT_NOT_READY` | `0x02` | 02 | **No Seek Complete** | ⚠️ semantic mismatch — see §4.3 |
| `BAD_FORMAT` | `0x2C` | **1C** | Unformatted/Bad Format | ❌ **WRONG — see §4.4** |
| `INTERLEAVE_ERROR` | `0x2A` | **1A** | Interleave Error | ❌ **WRONG — see §4.4** |

### 4.3 `UNIT_NOT_READY = 0x02` — semantic mismatch · LOW
`UNIT_NOT_READY` is returned whenever a LUN is not started (TEST UNIT READY 567, REZERO 599, SEEK 1223, F-CODE 1944, BSSELECT 2164). Code `0x02` in the manual is **"No Seek Complete"**, a *drive* error; the manual's code for a not-ready unit is **`0x04` Drive Not Ready**. Returning `0x02` for "LUN not started" is misleading, though `0x02` is at least a valid Class-00 code so a host won't see an "unassigned" value. For TEST UNIT READY specifically, the manual (§6.3.1.1) names the expected errors as Drive Not Ready (04) and Write Fault (03) — so `0x04` would be the compliant choice.

### 4.4 `BAD_FORMAT = 0x2C` and `INTERLEAVE_ERROR = 0x2A` — wrong constants · HIGH / LOW · ✅ FIXED (both now `0x1C` / `0x1A`)
- **Manual Table 6-6:** Unformatted or Bad Format = **`1C`**; Interleave Error = **`1A`**. Both are Class-01 ("target") error codes.
- **`scsi.c:88-89`:** `#define BAD_FORMAT (0x2Cu<<24)` and `#define INTERLEAVE_ERROR (0x2Au<<24)`. `0x2C` and `0x2A` fall in the manual's Class-02 range, where Table 6-7 lists **26–2F as "Not Assigned"**.
- The source itself proves the intent: line 782 `requestSenseData[...] = BAD_FORMAT; // 1C Bad format`, line 701 comment "Unformatted or Bad format", line 717 comment "send a 1A error code (Interleave Error)". The comments say `1C`/`1A`; the macros say `0x2C`/`0x2A`. Someone set bit 5 (`+0x10`) by mistake, bumping both from Class-01 to an unassigned Class-02 value.
- **Impact:** `BAD_FORMAT` is *actively emitted* — FORMAT failure (701, 782), READ6/WRITE6/TRANSLATE auto-start failure (956, 1102, 1284). A host decoding the sense byte sees "system error 2C, not assigned" instead of "bad format". `INTERLEAVE_ERROR` is only referenced in commented-out code (726), so it is latent.
- **Fix:** `#define BAD_FORMAT (0x1Cu<<24)` and `#define INTERLEAVE_ERROR (0x1Au<<24)`.

### 4.5 Completion Status byte — COMPLIANT (for the implemented subset)
- Manual §6.4 / Figure 6-31: bit 1 = Check Condition, bit 2 = Equal, bit 3 = Busy, bits 0/5/6/7 = 0.
- `scsi.c` uses only `0x00` (Good) and `0x02` (Check Condition, `SCSI_STATUS_CHECK_COND`, line 105). ✅ for bit 1.
- Bit 2 (Equal) is never set — correct, since SEARCH DATA EQUAL is not implemented.
- Bit 3 (Busy) is never set — acceptable, since the emulation has no asynchronous SEEK and is never "reserved".
- `0x25` "Invalid Logical Unit Number" (manual Table 6-7) is never used — an unavailable LUN is reported via `UNIT_NOT_READY`/`DRIVE_NOT_READY`/`BAD_FORMAT` instead. Minor.

---

## 5. Drive geometry & parameters

### 5.1 MODE SELECT parameter layout — COMPLIANT
Manual §6.3.1.11: MODE SELECT data = 4-byte parameter list (bytes 0-2 reserved 0, byte 3 = extent-descriptor length = 8) + 8-byte extent descriptor + 10-byte drive parameter list = 22 bytes (`0x16`). `scsi.c:1453` tests `(length == 22) && Buffer[3] == 8`, then `start = 4 + 8 = 12` and writes the 10-byte (`22-4-8`) drive parameter list (1457-1459). ✅ The wire layout matches the manual exactly. Standalone mode pages (non-22-byte) are handled by a separate page loop (1472-1499).

### 5.2 Extent descriptor block size — COMPLIANT in transit
Manual §6.3.1.11: extent descriptor bytes 5-7 = block size, must be 256/512/1024. `scsi.c` passes the descriptor through to `filesystemWriteModePageData` without validating the 256/512/1024 constraint — the manual says a violation should give error `0x24`. Minor deviation (no range check), but the block size itself is preserved.

### 5.3 `.dsc` descriptor parsing — `filesystem.c:720-792`
The legacy `.dsc` LUN-descriptor reader takes a 22-byte file: BlockSize ← bytes 9-11, Cylinders ← bytes 13-14, Heads ← byte 15, SectorsPerTrack ← `DEFAULT_SECTORS_PER_TRACK` (33, since `.dsc` files don't store it — the comment at filesystem.c:783 cites "F-2 in the ACB-4000 manual", the 33-sector 2:1-interleave format). This is BeebSCSI's own descriptor format, consistent with the geometry maths but not itself defined by the ACB-4000 manual.

### 5.4 Drive-parameter defaults
The manual §6.3.1.11 gives defaults: 306 cylinders, 2 heads, RWC cylinder 150, etc. BeebSCSI does not hard-code these — geometry comes from the `.dsc`/`.cfg` files. Not a deviation (the manual's defaults apply only when the host omits the parameter list); just noting the values are file-driven.

### 5.5 TRANSLATE response — COMPLIANT
Manual Figure 6-11: 8-byte response = 3 bytes cylinder + 1 byte head + 4 bytes "bytes from index". `scsi.c:1367-1376` emits exactly that. CHS maths (1350-1352) uses `C = LBA/(HPC·SPT)`, `H = (LBA/SPT) mod HPC`, `bytesFromIndex = (LBA mod SPT + 1)·blockSize` — a valid LBA→CHS mapping. ✅

---

## 6. Deviation summary (ranked) — with fix status

| # | Severity | Location | Deviation | Status |
|---|---|---|---|---|
| 1 | HIGH | `scsi.c:86` | `BAD_FORMAT = 0x2C`; manual code is `1C`. Actively emitted on every format/auto-start failure. | ✅ **FIXED** — now `0x1Cu<<24` |
| 2 | HIGH | `scsi.c:1807` | READ CAPACITY returned block *count* `N`, manual wants last-block address `N−1` | ✅ **FIXED** — now `filesystemGetLunTotalSectors(...) - 1` |
| 3 | MEDIUM | `scsi.c` (READ CAPACITY) | PMI byte 8 never validated; manual requires `0x24` for values other than 00/01 | ✅ **FIXED** — `data[8]` validated, rejects with `BAD_ARG` |
| 4 | MEDIUM | `scsi.c:1662` | START/STOP tested the whole of byte 4, not bit 0 | ✅ **FIXED** — now `(data[4] & 0x01) == 0` |
| 5 | MEDIUM | `scsi.c` MODE SENSE | MODE SENSE overrides parameter-header byte 0 (SCSI-2 length) instead of the manual's reserved-zero | ⚠️ **NOT FIXED — deliberate**; see note below |
| 6 | MEDIUM | `scsi.c` VERIFY | VERIFY ignores the block count — a verify spanning past end-of-LUN passes | ⚠️ **NOT FIXED — reverted**; see note below |
| 7 | MEDIUM | `scsi.c` SEND DIAGNOSTIC | SEND DIAGNOSTIC stub never enters DATA OUT phase to consume the parameter list | ⚠️ **NOT FIXED — risky**; see note below |
| 8 | LOW | `scsi.c:84` | `UNIT_NOT_READY = 0x02` ("No Seek Complete"); a not-ready unit should be `0x04` | ⚠️ **NOT FIXED** — held; see note below |
| 9 | LOW | `scsi.c:89` | `INTERLEAVE_ERROR = 0x2A`; manual code is `1A` (latent — only in commented code) | ✅ **FIXED** — now `0x1Au<<24` |
| 10 | LOW | `scsi.c` FORMAT + filesystem.c | FORMAT fill pattern: byte 2 used unconditionally, and `filesystemFormatLun` ignores it; manual default `0x6C` never applied | ⚠️ **NOT FIXED** — needs a `filesystem.c` change; harmless for a file-backed emulation |
| 11 | LOW | whole file | Reserved bytes & control byte not checked (manual's strict mode rejects non-zero) | ⚠️ **NOT FIXED — intentional** — matches Appendix B "expanded option" |
| — | INFO | — | 7 ACB-4000 commands unimplemented (WRITE/READ BUFFER, RECEIVE DIAGNOSTIC, READ(10), WRITE(10), WRITE AND VERIFY, SEARCH DATA EQUAL) | implement only if a host needs them |
| — | INFO | — | 3 extra commands not in ACB-4000 (INQUIRY 12, REASSIGN BLOCKS 07, READ DEFECT DATA(10) 37) + vendor groups 6/7 | out of spec scope; harmless |

### Items deliberately not changed

- **#5 MODE SENSE byte 0** — emitting a SCSI-2-style "mode data length" in byte 0 (instead of the ACB-4000's reserved zero) is how BeebSCSI's data reaches the BBC-side ADFS driver. Changing it to the manual's reserved-zero would very likely break that driver. Left as-is; flagged so it is a conscious choice, not an oversight.
- **#6 VERIFY block count** — a range check on `start + count` was applied and then **reverted**. The function header explicitly documents the intended behaviour as a minimal check ("only verifies the LUN is available, and the LBAs given are within range"). A `count == 0` means 65536 blocks per manual §6.2.7, so the check would reject any `*VERIFY` that sends count 0 against a LUN smaller than 65536 sectors — and the BBC Master `*VERIFY` utility's exact behaviour is unverified. It is also not a corruption bug. Adding the check should be done only with testing against the real utility; reverted to avoid a blind regression.
- **#7 SEND DIAGNOSTIC** — the stub works because Acorn ADFS never sends a SEND DIAGNOSTIC parameter list. Blindly adding a DATA OUT phase risks hanging the bus if a host sends the command with no data. A correct fix would read the CDB byte 3-4 length and consume exactly that many bytes — worth doing only if a host is found that actually uses it.
- **#8 `UNIT_NOT_READY = 0x02`** — changing it to `0x04` would make it identical to `DRIVE_NOT_READY`, and ADFS's error handling may distinguish the two codes. Low impact; left for the maintainer to decide.
- **#10 / #11** — emulation-appropriate choices; see the rows above.

**Bottom line:** the HIGH/MEDIUM items that were safe to change have been fixed (#1, #2, #3, #4) plus the latent #9. Four items remain open (#5, #6, #7, #8) — each is either a deliberate ADFS-compatibility choice or carries a real regression risk that needs testing against the actual BBC host software, so they were left for a maintainer decision rather than changed blind. For the command subset it implements, `scsi.c` now follows the ACB-4000 CDB, data and sense formats faithfully.

---

*Compliance review 2026-05-23. Manual: ACB-4000 OEM Manual, Adaptec, March 1984. Source: `BeebSCSI/scsi.c` + `BeebSCSI/filesystem.c` as of this date. Line numbers refer to that revision.*
