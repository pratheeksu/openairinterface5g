# OAI PUCCH Symbol Allocation on Mixed Slots Investigation Report

## Executive Summary

OAI **DOES account for mixed slots** having only 4 UL symbols instead of 14. The mechanism involves:
1. A TDD slot bitmap structure tracking UL/DL symbol counts per slot
2. Symbol bitmaps created only for valid UL symbols in mixed slots
3. Explicit checks preventing PUCCH allocation on mixed slots with insufficient UL symbols

---

## 1. VRB_MAP_UL Population and Mixed Slot Handling

### 1.1 VRB_MAP_UL Structure
**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler.c#L100-L146)

```c
const int index = ul_buffer_index(frame, slot, fs->numb_slots_frame, nrmac->vrb_map_UL_size);
uint16_t *vrb_map_UL = &nrmac->common_channels[0].vrb_map_UL[beam][index * MAX_BWP_SIZE];

// Initialization: copy ulprbbl (all 0x3FFF = 14 symbols per PRB)
memcpy(&vrb_map_UL[prev_slot % size * MAX_BWP_SIZE], &gNB->ulprbbl, sizeof(uint16_t) * MAX_BWP_SIZE);
```

- **vrb_map_UL** is a **circular buffer indexed by slot number**
- Each PRB has a **16-bit bitmap** where each bit represents one OFDM symbol
- Initial value: `0x3FFF` = bits 0-13 set (all 14 symbols)
- **Each slot gets its own vrb_map_UL buffer**

### 1.2 Symbol Bitmap Function
**File:** [common/utils/nr/nr_common.c](common/utils/nr/nr_common.c#L967-L969)

```c
uint16_t SL_to_bitmap(int startSymbolIndex, int nrOfSymbols) {
  return ((1<<nrOfSymbols)-1)<<startSymbolIndex;
}
```

**Example calculations:**
- Format 0/1 on full UL slot (14 symbols, start 0): `SL_to_bitmap(0, 14)` = `0x3FFF` (bits 0-13)
- Format 0/1 on mixed slot (4 UL symbols, start 10): `SL_to_bitmap(10, 4)` = `0x3C00` (bits 10-13)

---

## 2. TDD Slot Bitmap Structure

### 2.1 Data Structure Definition
**File:** [common/utils/nr/nr_common.h](common/utils/nr/nr_common.h#L150-L165)

```c
typedef struct tdd_bitmap {
  enum slot_type slot_type;      // TDD_NR_DOWNLINK_SLOT, TDD_NR_UPLINK_SLOT, TDD_NR_MIXED_SLOT
  uint8_t num_dl_symbols;        // Number of DL symbols in mixed slot
  uint8_t num_ul_symbols;        // Number of UL symbols in mixed slot
} tdd_bitmap_t;

typedef struct tdd_period_config_s {
  tdd_bitmap_t tdd_slot_bitmap[MAX_NUM_SLOTS_ALLOWED];  // One per slot in TDD period
  uint8_t num_dl_slots;
  uint8_t num_ul_slots;
} tdd_period_config_t;
```

### 2.2 Slot Bitmap Population
**File:** [openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c](openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c#L90-L130)

```c
for (int i = 0; i < total_slot; i++) {
  tdd_bitmap_t *bitmap = &pc->tdd_slot_bitmap[i + curr_total_slot];
  if (i < n_dl_slot)
    bitmap->slot_type = TDD_NR_DOWNLINK_SLOT;
  else if ((i == n_dl_slot) && has_mixed_slot) {
    bitmap->slot_type = TDD_NR_MIXED_SLOT;
    bitmap->num_dl_symbols = n_dl_symbols;    // E.g., 10 for slot 7
    bitmap->num_ul_symbols = n_ul_symbols;    // E.g., 4 for slot 7
  } else if (n_ul_slot)
    bitmap->slot_type = TDD_NR_UPLINK_SLOT;
}
```

**Example (Band 78, TDD pattern 10 DL + 4 UL symbols + 2 guard):**
- Slot 7 (mixed): `num_dl_symbols=10, num_ul_symbols=4`
- UL symbols occupy indices 10-13 (symbols after DL and guard periods)

### 2.3 Symbol Validation Functions
**File:** [openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c](openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c#L13-L25)

```c
uint16_t get_ul_bitmap(const frame_structure_t *fs, int slot) {
  const int slot_period = slot % fs->numb_slots_period;
  const tdd_bitmap_t *bm = &fs->period_cfg.tdd_slot_bitmap[slot_period];
  /* For mixed slots, only store the number of symbols; for full UL, assume 14 */
  const int num_ul_symbols = bm->slot_type == TDD_NR_MIXED_SLOT ? bm->num_ul_symbols : 14;
  return SL_to_bitmap(14 - num_ul_symbols, num_ul_symbols);
}

bool is_ul_slot(const slot_t slot, const frame_structure_t *fs) {
  if (fs->frame_type == FDD)
    return true;
  const tdd_period_config_t *pc = &fs->period_cfg;
  slot_t s = get_slot_idx_in_period(slot, fs);
  return ((is_mixed_slot(s, fs) && pc->tdd_slot_bitmap[s].num_ul_symbols)
          || (pc->tdd_slot_bitmap[s].slot_type == TDD_NR_UPLINK_SLOT));
}
```

**KEY:** `get_ul_bitmap()` automatically adjusts for mixed slots:
- Mixed slot 7 with 4 UL symbols → returns `0x3C00` (symbols 10-13 only)
- Full UL slot → returns `0x3FFF` (symbols 0-13)

---

## 3. Functions That Initialize/Mark Available Symbols

### 3.1 VRB_MAP_UL Initialization for PUCCH

**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1040-L1090)

```c
static bool test_pucch0_vrb_occupation(const NR_sched_pucch_t *pucch, uint16_t *vrb_map_UL, const int bwp_start) {
  // Check if each symbol of PUCCH is free
  for (int l=0; l<pucch->nr_of_symb; l++) {
    uint16_t symb = SL_to_bitmap(pucch->start_symb+l, 1);  // Single-symbol bitmap
    int prb = (l==1 && pucch->second_hop_prb != 0) ? pucch->second_hop_prb : pucch->prb_start;
    if ((vrb_map_UL[bwp_start+prb] & symb) != 0) {  // Bitwise AND check
      return false;  // Symbol occupied
    }
  }
  return true;
}

static void set_pucch0_vrb_occupation(const NR_sched_pucch_t *pucch, uint16_t *vrb_map_UL, const int bwp_start) {
  // Mark each PUCCH symbol as occupied
  for (int l=0; l<pucch->nr_of_symb; l++) {
    uint16_t symb = SL_to_bitmap(pucch->start_symb+l, 1);
    int prb = (l==1 && pucch->second_hop_prb != 0) ? pucch->second_hop_prb : pucch->prb_start;
    vrb_map_UL[bwp_start+prb] |= symb;  // Bitwise OR to mark occupied
  }
}
```

**Operation:**
- Tests **symbol-by-symbol** for occupation
- Uses **bitwise AND** to check if symbol bits are set in vrb_map_UL
- Uses **bitwise OR** to mark symbols as occupied after allocation

### 3.2 CSI PUCCH Resource Allocation
**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L260-L310)

```c
switch(pucchres->format.present) {
  case NR_PUCCH_Resource__format_PR_format2:
    len = pucchres->format.choice.format2->nrofPRBs;
    mask = SL_to_bitmap(pucchres->format.choice.format2->startingSymbolIndex, 
                        pucchres->format.choice.format2->nrofSymbols);
    break;
  // ... other formats ...
}

// Verify resources are free
for (int i = start; i < start + len; ++i) {
  if((vrb_map_UL[i+bwp_start] & mask) != 0) {
    LOG_E(NR_MAC, "VRB MAP in not free. Can't schedule CSI reporting on PUCCH.\n");
    memset(curr_pucch, 0, sizeof(*curr_pucch));
  } else
    vrb_map_UL[i+bwp_start] |= mask;  // Mark occupied
}
```

---

## 4. CRITICAL: Symbol Constraints in Mixed Slots

### 4.1 Mixed Slot PUCCH Symbol Check
**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1165-L1180)

```c
// Within HARQ ACK/NACK scheduling
for (...) {
  ...
  const int pucch_slot = (slot + pdsch_to_harq_feedback[f] + NTN_gNB_Koffset) % n_slots_frame;
  
  if (fs->frame_type == TDD) {
    int mod_slot = get_slot_idx_in_period(pucch_slot, fs);
    if (!is_ul_slot(mod_slot, fs))
      continue;  // Skip non-UL slots
    
    const tdd_period_config_t *pc = &fs->period_cfg;
    if (r_pucch >= 0 && is_mixed_slot(mod_slot, fs) && pc->tdd_slot_bitmap[mod_slot].num_ul_symbols < 2)
      continue;  // *** EXPLICIT CHECK: Skip if mixed slot has < 2 UL symbols ***
  }
  ...
}
```

**Impact:**
- **For PUCCH Format 0/1** (which use common resources): Cannot be scheduled on mixed slots with fewer than 2 UL symbols
- **Mixed slot 7** (4 UL symbols): Passes this check (4 ≥ 2)
- **Mixed slot with 1 UL symbol**: Would be skipped entirely

### 4.2 SR (Scheduling Request) Scheduling
**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1273-L1350)

```c
void nr_sr_reporting(gNB_MAC_INST *nrmac, frame_t SFN, slot_t slot) {
  if (!is_ul_slot(slot, &nrmac->frame_structure))
    return;  // Only process UL slots or mixed slots with UL symbols
  
  // For each SR configuration ...
  for (int SR_resource_id = 0; SR_resource_id < ...; SR_resource_id++) {
    ...
    set_pucch_allocation(ul_bwp, -1, bwp_size, curr_pucch);  // Get Format 0/1 symbols
    bool ret = test_pucch0_vrb_occupation(curr_pucch, vrb_map_UL, bwp_start);
    if (!ret) {
      LOG_E(NR_MAC,"Cannot schedule SR. PRBs not available\n");
      continue;
    }
    ...
    set_pucch0_vrb_occupation(curr_pucch, vrb_map_UL, bwp_start);  // Mark occupied
  }
}
```

**Flow:**
1. Check if slot is UL-capable (includes mixed slots with UL symbols)
2. Get PUCCH Format 0/1 allocation (uses configuration's start symbol)
3. Test if symbols are available in vrb_map_UL
4. If successful, mark symbols as occupied

---

## 5. What Happens If PUCCH Requests Symbols Outside UL Range?

### Scenario: Mixed Slot 7 (UL symbols 10-13 only)

**If PUCCH Format 0/1 configured for symbols 0-2:**
1. `test_pucch0_vrb_occupation()` creates bitmap `SL_to_bitmap(0, 2)` = `0x0003` (bits 0-1)
2. Checks: `vrb_map_UL[prb] & 0x0003`
3. **Result:** If vrb_map_UL bits 0-1 are marked as occupied (they would be from ulprbbl init), allocation fails
4. **Log:** "Cannot schedule SR. PRBs not available"

**If PUCCH Format 0/1 configured for symbols 10-13:**
1. Creates bitmap `SL_to_bitmap(10, 2)` = `0x0C00` (bits 10-11)
2. Checks available symbols in vrb_map_UL
3. **Result:** Will succeed if symbols 10-11 are free

### Configuration-Driven Behavior
**File:** [openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1500-L1520)

```c
static void set_pucch_allocation(const NR_UE_UL_BWP_t *ul_bwp, const int r_pucch, const int bwp_size, NR_sched_pucch_t *pucch) {
  if(r_pucch<0) {
    // Use dedicated PUCCH resources configured in RRC
    const NR_PUCCH_Resource_t *resource = ul_bwp->pucch_Config->resourceToAddModList->list.array[0];
    pucch->nr_of_symb = resource->format.choice.format0->nrofSymbols;
    pucch->start_symb = resource->format.choice.format0->startingSymbolIndex;  // From config
    pucch->prb_start = resource->startingPRB;
  } else {
    // Use common PUCCH resources via set_r_pucch_parms()
    set_r_pucch_parms(rsetindex, r_pucch, bwp_size, &pucch->prb_start, &pucch->second_hop_prb, &pucch->nr_of_symb, &pucch->start_symb);
  }
}
```

**Key:** The starting symbol index comes from **RRC configuration**, not dynamically adjusted for mixed slots. This means:
- Misconfigured PUCCH resources for mixed slots **will fail allocation**
- Properly configured resources (symbols within UL range) will succeed

---

## 6. Code Flow: Complete Example for SR on Mixed Slot 7

### Scenario
- **Slot 7 (mixed):** 10 DL symbols (0-9) + 2 guard symbols (placeholder concept) + 4 UL symbols (10-13)
- **PUCCH Format 0/1 configured:** Start symbol 10, 2 symbols → `SL_to_bitmap(10, 2)` = `0x0C00`

### Execution Flow

```
1. nr_sr_reporting() called
   └─ is_ul_slot(7, fs) → TRUE (mixed slot with 4 UL symbols)
   
2. For each SR resource:
   └─ get_pucch_index() → gets slot index in PUCCH period
   
3. get_pucch_allocation():
   └─ start_symb = 10, nr_of_symb = 2 (from config)
   
4. test_pucch0_vrb_occupation():
   └─ Loop l=0,1:
      ├─ l=0: symb = SL_to_bitmap(10, 1) = 0x0400 (bit 10)
      │  Check: (vrb_map_UL[prb] & 0x0400) != 0?
      │  If bit 10 NOT set → OK
      │
      └─ l=1: symb = SL_to_bitmap(11, 1) = 0x0800 (bit 11)
         Check: (vrb_map_UL[prb] & 0x0800) != 0?
         If bit 11 NOT set → OK
   
5. If all symbols free:
   └─ set_pucch0_vrb_occupation():
      ├─ vrb_map_UL[prb] |= 0x0400  (mark symbol 10 occupied)
      └─ vrb_map_UL[prb] |= 0x0800  (mark symbol 11 occupied)
   
6. nr_fill_nfapi_pucch():
   └─ Configure PHY with PUCCH PDU for symbols 10-11
```

---

## 7. Key Functions Summary

| Function | File | Purpose | Mixed Slot Handling |
|----------|------|---------|-------------------|
| `get_ul_bitmap()` | nr_mac_common_tdd.c | Returns valid UL symbol range | Checks `num_ul_symbols`, returns correct bitmap |
| `is_ul_slot()` | nr_mac_common_tdd.c | Checks if slot has UL symbols | Returns true for mixed slots with `num_ul_symbols > 0` |
| `SL_to_bitmap()` | nr_common.c | Creates symbol bitmap | No special handling (works for any range) |
| `test_pucch0_vrb_occupation()` | gNB_scheduler_uci.c | Tests symbol availability | Per-symbol bitwise AND check |
| `set_pucch0_vrb_occupation()` | gNB_scheduler_uci.c | Marks symbols occupied | Per-symbol bitwise OR |
| `nr_sr_reporting()` | gNB_scheduler_uci.c | Schedules SR | Calls `is_ul_slot()` first |
| `nr_schedule_pucch()` | gNB_scheduler_uci.c | Main PUCCH scheduler | Calls `is_ul_slot()` first |

---

## 8. Explicit Mixed Slot Constraints

### PUCCH Format 0/1 Allocation Restriction
**Line 1174 in gNB_scheduler_uci.c:**
```c
if (r_pucch >= 0 && is_mixed_slot(mod_slot, fs) && pc->tdd_slot_bitmap[mod_slot].num_ul_symbols < 2)
  continue;  // SKIP if mixed slot has fewer than 2 UL symbols
```

**Interpretation:**
- **r_pucch >= 0:** Using common PUCCH resources
- **is_mixed_slot():** Current slot is mixed
- **num_ul_symbols < 2:** Mixed slot has 0 or 1 UL symbol

**Result:** Cannot use common PUCCH resources on mixed slots with < 2 UL symbols

### Why This Matters
- **Mixed slot 7 (4 UL symbols):** Allowed (4 ≥ 2)
- **Hypothetical slot with 1 UL symbol:** NOT allowed for common resources

---

## 9. References to Code

### Data Structures
- [TDD Bitmap Structure](common/utils/nr/nr_common.h#L150-L165)
- [Frame Structure](common/utils/nr/nr_common.h#L166-L172)

### Initialization
- [VRB_MAP_UL Init](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler.c#L140-L150)
- [TDD Slot Bitmap Config](openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c#L90-L130)

### Symbol Allocation
- [get_ul_bitmap()](openair2/LAYER2/NR_MAC_COMMON/nr_mac_common_tdd.c#L13-L17)
- [PUCCH VRB Occupation Test](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1050-L1085)
- [PUCCH VRB Occupation Set](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1090-L1105)

### Scheduling
- [SR Reporting](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1273-L1350)
- [Mixed Slot Check](openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_uci.c#L1174)

---

## 10. Conclusions

1. **VRB_MAP_UL accounts for mixed slots:** Yes, through configuration-driven symbol ranges and per-slot indexing
2. **Symbol availability initialization:** Proper symbols marked via `get_ul_bitmap()` which accounts for `num_ul_symbols`
3. **Mixed slot constraints:** Explicit check prevents PUCCH Format 0/1 on mixed slots with < 2 UL symbols
4. **Symbol-by-symbol checking:** Uses bitwise operations to validate each symbol individually
5. **Resource marking:** Uses bitwise OR to mark occupied symbols per symbol, not per slot
6. **Misconfiguration handling:** Properly configured resources work; misconfigured resources fail allocation with diagnostic logs
