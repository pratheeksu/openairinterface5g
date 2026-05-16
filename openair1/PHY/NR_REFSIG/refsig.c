/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nr_refsig.h"
#include "openair1/PHY/gold.h"

/* ── Gold sequence cache: open-addressing hash table (thread-local) ── */

// align sequence length to 64 bytes for AVX-512
static const int grain = 64 / sizeof(uint32_t); // 16

typedef struct {
  uint32_t key;
  int length;    // cached sequence length (uint32_t words, grain-aligned)
  uint32_t *seq; // 64-byte aligned sequence data (NULL = empty slot)
} gold_ht_entry_t;

typedef struct {
  gold_ht_entry_t *entries;
  int capacity; // power of 2
  int count;
} gold_ht_t;

#define GOLD_HT_INIT_CAP 256 // must be power of 2

static inline uint32_t gold_hash(uint32_t key)
{
  // multiplicative hash (Knuth)
  return key * 2654435761u;
}

static uint32_t *gold_generate(uint32_t key, int length)
{
  uint32_t *seq;
  int ret = posix_memalign((void **)&seq, 64, length * sizeof(uint32_t));
  AssertFatal(ret == 0, "gold_generate: out of memory\n");
  unsigned int x1 = 0, x2 = key;
  seq[0] = gold_generic(&x1, &x2, 1);
  for (int n = 1; n < length; n++)
    seq[n] = gold_generic(&x1, &x2, 0);
  return seq;
}

static void gold_ht_grow(gold_ht_t *ht)
{
  int old_cap = ht->capacity;
  gold_ht_entry_t *old = ht->entries;

  ht->capacity = old_cap ? old_cap * 2 : GOLD_HT_INIT_CAP;
  ht->entries = calloc(ht->capacity, sizeof(gold_ht_entry_t));
  AssertFatal(ht->entries, "gold_ht_grow: out of memory\n");
  ht->count = 0;

  // rehash existing entries
  uint32_t mask = ht->capacity - 1;
  for (int i = 0; i < old_cap; i++) {
    if (!old[i].seq)
      continue;
    uint32_t idx = gold_hash(old[i].key) & mask;
    while (ht->entries[idx].seq)
      idx = (idx + 1) & mask;
    ht->entries[idx] = old[i];
    ht->count++;
  }
  free(old);
}

static pthread_key_t gold_table_key;
static pthread_once_t gold_key_once = PTHREAD_ONCE_INIT;

static void delete_table(void *ptr)
{
  gold_ht_t *ht = (gold_ht_t *)ptr;
  if (ht->entries) {
    for (int i = 0; i < ht->capacity; i++)
      free(ht->entries[i].seq); // free(NULL) is safe
    free(ht->entries);
  }
  free(ht);
}

static void make_table_key(void)
{
  (void)pthread_key_create(&gold_table_key, delete_table);
}

uint32_t *gold_cache(uint32_t key, int length)
{
  (void)pthread_once(&gold_key_once, make_table_key);
  gold_ht_t *ht;
  if ((ht = pthread_getspecific(gold_table_key)) == NULL) {
    ht = calloc(1, sizeof(gold_ht_t));
    gold_ht_grow(ht); // allocate initial table
    (void)pthread_setspecific(gold_table_key, ht);
  }

  // align length for AVX-512
  length = ((length + grain - 1) / grain) * grain;

  uint32_t mask = ht->capacity - 1;
  uint32_t idx = gold_hash(key) & mask;

  // probe for existing entry
  while (ht->entries[idx].seq) {
    if (ht->entries[idx].key == key) {
      if (ht->entries[idx].length >= length)
        return ht->entries[idx].seq;
      // same key but need longer sequence: regenerate
      free(ht->entries[idx].seq);
      ht->entries[idx].seq = gold_generate(key, length);
      ht->entries[idx].length = length;
      return ht->entries[idx].seq;
    }
    idx = (idx + 1) & mask;
  }

  // grow if load factor > 75%
  if (ht->count * 4 >= ht->capacity * 3) {
    gold_ht_grow(ht);
    // re-probe after grow
    mask = ht->capacity - 1;
    idx = gold_hash(key) & mask;
    while (ht->entries[idx].seq)
      idx = (idx + 1) & mask;
  }

  // insert new entry
  ht->entries[idx].key = key;
  ht->entries[idx].length = length;
  ht->entries[idx].seq = gold_generate(key, length);
  ht->count++;
  LOG_D(PHY, "gold_cache: new entry key=%u len=%d (table %d/%d)\n", key, length, ht->count, ht->capacity);
  return ht->entries[idx].seq;
}

uint32_t *nr_gold_pbch(int Lmax, int Nid, int n_hf, int l)
{
  int i_ssb = l & (Lmax - 1);
  int i_ssb2 = i_ssb + (n_hf << 2);
  uint32_t x2 = (1 << 11) * (i_ssb2 + 1) * ((Nid >> 2) + 1) + (1 << 6) * (i_ssb2 + 1) + (Nid & 3);
  return gold_cache(x2, NR_PBCH_DMRS_LENGTH_DWORD);
}

uint32_t *nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short nid, int ns, int l)
{
  int pdcch_dmrs_init_length = (((N_RB_DL << 1) * 3) >> 5) + 1;
  uint64_t x2tmp0 = (((uint64_t)symbols_per_slot * ns + l + 1) * ((nid << 1) + 1));
  x2tmp0 <<= 17;
  x2tmp0 += (nid << 1);
  uint32_t x2 = x2tmp0 % (1U << 31); // cinit
  LOG_D(PHY, "PDCCH DMRS slot %d, symb %d, Nid %d, x2 %x\n", ns, l, nid, x2);
  return gold_cache(x2, pdcch_dmrs_init_length);
}

uint32_t *nr_gold_pdsch(int N_RB_DL, int symbols_per_slot, int nid, int nscid, int slot, int symbol)
{
  int pdsch_dmrs_init_length = ((N_RB_DL * 24) >> 5) + 1;
  uint64_t x2tmp0 = (((uint64_t)symbols_per_slot * slot + symbol + 1) * (((uint64_t)nid << 1) + 1)) << 17;
  uint32_t x2 = (x2tmp0 + (nid << 1) + nscid) % (1U << 31); // cinit
  LOG_D(PHY, "UE DMRS slot %d, symb %d, nscid %d, x2 %x\n", slot, symbol, nscid, x2);
  return gold_cache(x2, pdsch_dmrs_init_length);
}

uint32_t *nr_gold_pusch(int N_RB_UL, int symbols_per_slot, int Nid, int nscid, int slot, int symbol)
{
  return nr_gold_pdsch(N_RB_UL, symbols_per_slot, Nid, nscid, slot, symbol);
}

uint32_t *nr_gold_csi_rs(int N_RB_DL, int symbols_per_slot, int slot, int symb, uint32_t Nid)
{
  int csi_dmrs_init_length = ((N_RB_DL << 4) >> 5) + 1;
  uint64_t temp_x2 = (1ULL << 10) * ((uint64_t)symbols_per_slot * slot + symb + 1) * ((Nid << 1) + 1) + Nid;
  uint32_t x2 = temp_x2 % (1U << 31);
  return gold_cache(x2, csi_dmrs_init_length);
}

uint32_t *nr_gold_prs(int Nid, int slotNum, int symNum)
{
  LOG_D(PHY, "Initialised NR-PRS sequence for PCI %d\n", Nid);
  // initial x2 for prs as ts138.211
  uint32_t pow22 = 1 << 22;
  uint32_t pow10 = 1 << 10;
  uint32_t c_init1 = pow22 * ceil(Nid / 1024);
  uint32_t c_init2 = pow10 * (slotNum + symNum + 1) * (2 * (Nid % 1024) + 1);
  uint32_t c_init3 = Nid % 1024;
  uint32_t x2 = c_init1 + c_init2 + c_init3;
  return gold_cache(x2, NR_MAX_PRS_INIT_LENGTH_DWORD);
}
