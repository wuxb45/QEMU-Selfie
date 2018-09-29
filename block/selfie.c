/*
 * Block Driver for Selfie image format
 *
 * Copyright (c) 2014 Wu, Xingbo <wuxb45@gmail.com>
 *
 */
// {{{ #include
#include <lz4.h>

#include "qemu-common.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/iov.h"
// }}}
// {{{ Macros
// unused/z/n/l2
#define ZONE_TYPE_0 ((0))
#define ZONE_TYPE_Z ((1))
#define ZONE_TYPE_N ((2))
#define ZONE_TYPE_L ((3))

#define INIT_NONE ((0))
#define INIT_TRIM ((1))
#define INIT_ZERO ((2))

// only used for l1/l2
#define SELFIE_PAGE_SIZE ((UINT64_C(4096)))
// }}}
// {{{ structs
static const uint8_t SELFIE_MAGIC[8] = {'Z','B','D','M','A','G','I','C'};

// header (1) | zone_info (?) | l1_pages (nr_l1) | zones (data/l2)...
// selfie metafile header (Immutable)
struct __attribute__((packed)) SelfieHeader {
  uint8_t  magic[8];
  uint64_t capacity;
  uint64_t block_shift; // block_size = 1<<block_shift
  uint64_t nr_l1; // image size = block_size * 512 * 512 * nr_l1
  uint64_t zone_size; // size of each zone
  uint64_t nr_zones;  // maximum number of zones
  uint64_t pa_zi;  // offset of zone info
  uint64_t pa_l1;  // offset of l1 tables
  uint64_t pa_zones; // start of data/l2 zones
  uint64_t init_type; // none/trim/zero
  //struct   timespec ts;
};

// buffered if allocated from z-zone
// write immediately if allocated from n-zone
struct SelfieIndexL1 {
  CoMutex write_lock;
  bool   dirty1;      // l1 is dirty;
  uint64_t * l1_page; // *512
  bool   dirty2[512]; // each l2 in l1 is dirty?
  uint64_t * l2_pages[512];
};

//// Zone
// No update on allocation for z-zone.
// sync-write on allocation for n-zone.
struct SelfieZoneInfo {
  uint32_t n:30; // next_id;
  uint32_t t:2;  // 0: unused, 1: z-zone, 2: n-zone
};

#define I_LOCK_SCALE ((64))
struct SelfieState {
  struct SelfieHeader header; // read from image on open, never rewrite
  BlockDriverState * main; // the file
  struct SelfieIndexL1 * nodes; // [header.nr_l1]
  struct SelfieZoneInfo * zones; // [header.nr_zones]
  uint64_t id_zzone; // current z-zone
  uint64_t id_nzone; // current n-zone
  uint64_t id_lzone; // current l-zone
  uint64_t block_size; // 1<<block_shift (aligned to 4KB)
  uint64_t zdata_size; // maximum data size after compression
  uint64_t zbuffer_size; // block_size + max_compression_size (+aligned to 4KB)
  uint64_t nr_zone_unit; // for alloc data
  uint64_t nr_zone_page; // for alloc l2
  // locks
  CoMutex index_lock[I_LOCK_SCALE];
  CoMutex zone_lock;
  // below is for debug
  int fd_log;
  uint64_t nr_write_data_z;
  uint64_t nr_write_data_n;
  uint64_t nr_write_zone;
  uint64_t nr_write_l1;
  uint64_t nr_write_l2;
};

struct __attribute__((packed)) SelfiePageHead {
  uint64_t va;
  uint16_t zsize; // <= 4096-10
  uint8_t zdata[];
};

struct SelfieZPage {
  union {
    struct SelfiePageHead zh;
    uint8_t buf[0];
  };
};
// }}}
// {{{ lock/unlock, logging
  static inline void
__lock(CoMutex * const lock)
{
  if (qemu_in_coroutine()) qemu_co_mutex_lock(lock);
}

  static inline void
__unlock(CoMutex * const lock)
{
  if (qemu_in_coroutine()) qemu_co_mutex_unlock(lock);
}

  static void
selfie_log(struct SelfieState * const s, const char * const msg, ...)
{
  if (s->fd_log < 0) return;

  char head[1024];
  char tail[1024];
  sprintf(head, "[]");

  va_list varg;
  va_start(varg, msg);
  vsnprintf(tail, sizeof(tail), msg, varg);
  va_end(varg);
  dprintf(s->fd_log, "%s%s\n", head, tail);
}

  static inline void
selfie_log_addr(struct SelfieState * const s, const char * const tag, const uint64_t start, const uint64_t size)
{
#if 0
  const uint64_t gb = size >> 30;
  const uint64_t mb = (size >> 20) & 0x3ff;
  const uint64_t kb = (size >> 10) & 0x3ff;
  const uint64_t b = size & 0x3ff;
  char buf[4][16];
  sprintf(buf[0], "%"PRIu64"GB+", gb);
  sprintf(buf[1], "%"PRIu64"MB+", mb);
  sprintf(buf[2], "%"PRIu64"KB+", kb);
  sprintf(buf[3], "%"PRIu64"B", b);
  selfie_log(s, "[%-20s]: @%12"PRIx64"[%12"PRIx64"] %s%s%s%s",
      tag, start, size, gb?buf[0]:"", mb?buf[1]:"", kb?buf[2]:"", b?buf[3]:"");
#endif
  return;
}

// }}}
// {{{ image read/write
// read n 4KB page from image
// buf should be aligned by 4KB
// return number of bytes read
  static int
image_pread(struct SelfieState * const s, const uint64_t pa, void * const buf, const uint64_t len)
{
  selfie_log_addr(s, "|---->R_IMAGE", pa, len);
  assert((pa % SELFIE_PAGE_SIZE) == 0);
  assert((len % SELFIE_PAGE_SIZE) == 0);
  const int rr = bdrv_pread(s->main, pa, buf, len);
  if (rr != len) { bzero(buf, len); }
  return (int)len;
}

// return number of bytes written
  static int
image_pwrite(struct SelfieState * const s, const uint64_t pa, const void * const buf, const uint64_t len)
{
  selfie_log_addr(s, "|---->W_IMAGE", pa, len);
  assert((pa % SELFIE_PAGE_SIZE) == 0);
  assert((len % SELFIE_PAGE_SIZE) == 0);
  return bdrv_pwrite(s->main, pa, buf, len);
}
// }}}
// {{{ zpage coding
// raw[SELFIE_PAGE_SIZE] -> zpage
  static bool
zpage_encode(struct SelfieState * const s, const uint8_t * const raw, struct SelfieZPage * const zpage, const uint64_t va)
{
  assert(zpage);
  zpage->zh.va = va;
  const int r = LZ4_compress_default((const char *)raw, (char *)(zpage->zh.zdata), SELFIE_PAGE_SIZE, s->zdata_size);
  if (r == 0) {
    return false;
  } else {
    zpage->zh.zsize = (typeof(zpage->zh.zsize))r;
    return true;
  }
}

// zpage -> raw data
  static bool
zpage_decode(struct SelfieState * const s, uint8_t * const raw, const struct SelfieZPage * const zpage)
{
  if (zpage->zh.zsize == 0) return false;
  assert(zpage->zh.zsize <= s->zdata_size);
  assert(zpage->zh.va < s->header.capacity);
  const int r = LZ4_decompress_safe((char *)(zpage->zh.zdata), (char *)raw, zpage->zh.zsize, SELFIE_PAGE_SIZE);
  assert(r == SELFIE_PAGE_SIZE);
  return true;
}
// }}}
// {{{ zone
// write zones[id] to the image
  static void
zone_sync(struct SelfieState * const s, const uint64_t id, const bool sync)
{
  assert(id < s->header.nr_zones);
  const uint64_t pa = s->header.pa_zi + (sizeof(s->zones[0]) * id);
  if (! s->main->read_only) {
    selfie_log_addr(s, "*ZONE_SYNC", pa, sizeof(s->zones[0]));
    const int r = bdrv_pwrite(s->main, pa, &(s->zones[id]), sizeof(s->zones[0]));
    assert(r == sizeof(s->zones[0]));
    if (sync && s->main->enable_write_cache) {
      bdrv_flush(s->main);
    }
  }
  atomic_inc(&(s->nr_write_zone));
}

// claim a unused zone to the given type and write to the image
  static void
zone_mark_sync(struct SelfieState * const s, const uint64_t id, const uint32_t type)
{
  selfie_log(s, "zone_marked: %u (type:%u (0znl))", id, type);
  s->zones[id].t = type;
  s->zones[id].n = 0;
  zone_sync(s, id, false);
}

// before using a new zone, call this to set all data as zeros
  static void
zone_write_zeroes(struct SelfieState * const s, const uint64_t id)
{
  if (s->main->read_only) return;
  const uint64_t size = s->header.zone_size;
  const uint64_t pa = s->header.pa_zones + (id * size);
  selfie_log_addr(s, "*ZONE_INIT", pa, size);
  switch (s->header.init_type) {
    case INIT_NONE:
      break;
    case INIT_ZERO:
      {
        const int r = bdrv_write_zeroes(s->main, pa>>9, size>>9, 0);
        assert(r == 0);
        bdrv_flush(s->main);
      }
      break;
    case INIT_TRIM:
      {
        const int r = bdrv_discard(s->main, pa>>9, size>>9);
        assert(r == 0);
      }
      break;
    default: break;
  }
}

// alloc a zone of given type
// find a 0,  convert to Z/N
  static bool
zone_alloc_type(struct SelfieState * const s, const uint32_t type)
{
  uint64_t start = 0;
  switch (type) {
    case ZONE_TYPE_Z: start = s->id_zzone; break;
    case ZONE_TYPE_N: start = s->id_nzone; break;
    case ZONE_TYPE_L: start = s->id_lzone; break;
    default: assert(false); break;
  }
  const uint64_t nr_zones = s->header.nr_zones;
  uint64_t i;
  for (i = start; i < nr_zones; i++) {
    if (s->zones[i].t == ZONE_TYPE_0) { // found unused zone.
      zone_mark_sync(s, i, type);
      switch (type) {
        case ZONE_TYPE_Z: s->id_zzone = i; break;
        case ZONE_TYPE_N: s->id_nzone = i; break;
        case ZONE_TYPE_L: s->id_lzone = i; break;
        default: assert(false); break;
      }
      zone_write_zeroes(s, i);
      return true;
    }
  }
  return false;
}

  static inline uint64_t
zone_id_to_pa(struct SelfieState * const s, const uint64_t zone_id, const uint64_t unit_id)
{
  uint64_t pa = 0;
  const uint64_t pa_base = s->header.pa_zones + (zone_id * s->header.zone_size);
  switch (s->zones[zone_id].t) {
    case ZONE_TYPE_Z: pa = pa_base + (unit_id * s->block_size); break;
    case ZONE_TYPE_N: pa = pa_base + (unit_id * s->block_size); break;
    case ZONE_TYPE_L: pa = pa_base + (unit_id * SELFIE_PAGE_SIZE); break;
    default: assert(false); break;
  }
  return pa;
}

  static uint32_t
zone_pa_type(struct SelfieState * const s, const uint64_t pa)
{
  assert(pa >= s->header.pa_zones);
  const uint64_t id = (pa - s->header.pa_zones) / s->header.zone_size;
  assert(id < s->header.nr_zones);
  return s->zones[id].t;
}

#if 0
  static const char *
zone_pa_type_str(struct SelfieState * const s, const uint64_t pa)
{
  switch (zone_pa_type(s, pa)) {
    case ZONE_TYPE_0: return "Unused";
    case ZONE_TYPE_Z: return "Z";
    case ZONE_TYPE_N: return "N";
    case ZONE_TYPE_L: return "L";
    default: return "Unknown";
  }
}
#endif

  static uint64_t
zone_alloc_z(struct SelfieState * const s, const uint64_t va_hint)
{
  __lock(&(s->zone_lock));
  if (s->zones[s->id_zzone].n == s->nr_zone_unit) { // full
    // TODO: write back index cluster, for fast scanning
    const bool ra = zone_alloc_type(s, ZONE_TYPE_Z);
    assert(ra);
  }
  const uint64_t id_zone = s->id_zzone;
  const uint64_t id_unit = s->zones[id_zone].n;
  s->zones[id_zone].n++;
  __unlock(&(s->zone_lock));
  const uint64_t pa = zone_id_to_pa(s, id_zone, id_unit);
  assert(zone_pa_type(s, pa) == ZONE_TYPE_Z);
  return pa;
}

  static uint64_t
zone_alloc_n(struct SelfieState * const s, const uint64_t va_hint)
{
  __lock(&(s->zone_lock));
  if (s->zones[s->id_nzone].n == s->nr_zone_unit) {
    const bool ra = zone_alloc_type(s, ZONE_TYPE_N);
    assert(ra);
  }
  const uint64_t id_zone = s->id_nzone;
  const uint64_t id_unit = s->zones[id_zone].n;
  s->zones[id_zone].n++;
  zone_sync(s, id_zone, false);
  __unlock(&(s->zone_lock));
  const uint64_t pa = zone_id_to_pa(s, id_zone, id_unit);
  assert(zone_pa_type(s, pa) == ZONE_TYPE_N);
  return pa;
}

  static uint64_t
zone_alloc_l(struct SelfieState * const s)
{
  __lock(&(s->zone_lock));
  if (s->zones[s->id_lzone].n == s->nr_zone_page) {
    const bool ra = zone_alloc_type(s, ZONE_TYPE_L);
    assert(ra);
  }
  const uint64_t id_zone = s->id_lzone;
  const uint64_t id_unit = s->zones[id_zone].n;
  s->zones[id_zone].n++;
  zone_sync(s, id_zone, false);
  // no sync on l-zone counter: scan on loading
  __unlock(&(s->zone_lock));
  const uint64_t pa = zone_id_to_pa(s, id_zone, id_unit);
  assert(zone_pa_type(s, pa) == ZONE_TYPE_L);
  return pa;
}
// }}}
// {{{ index mapping
// alloc l2 page in image file
  static uint64_t
index_l2_alloc(struct SelfieState * const s)
{
  const uint64_t pa = zone_alloc_l(s);
  selfie_log_addr(s, "*L2_ALLOC", pa, SELFIE_PAGE_SIZE);
  return pa;
}

  static void
index_write_id(struct SelfieState * const s, const uint64_t id_l1, const uint64_t id_l2)
{
  // update l2
  assert(id_l1 < s->header.nr_l1);
  assert(id_l2 < 512);
  struct SelfieIndexL1 * const node = &(s->nodes[id_l1]);
  __lock(&(node->write_lock));
  if (node->dirty2[id_l2]) {
    if (node->l1_page[id_l2] == 0) { // need alloc
      // set pa of l2 in l1
      node->l1_page[id_l2] = index_l2_alloc(s);
      node->dirty1 = true;
    }
    const uint64_t pa_l2 = node->l1_page[id_l2];
    assert(zone_pa_type(s, pa_l2) == ZONE_TYPE_L);
    assert(node->l2_pages[id_l2]);
    const int rw = image_pwrite(s, pa_l2, node->l2_pages[id_l2], SELFIE_PAGE_SIZE);
    assert(rw == SELFIE_PAGE_SIZE);
    selfie_log_addr(s, "*L2_WRITE", node->l1_page[id_l2], SELFIE_PAGE_SIZE);
    atomic_inc(&(s->nr_write_l2));
    // clear  dirty2
    node->dirty2[id_l2] = false;
  }

  // update l1
  if (node->dirty1) {
    // write l1
    const uint64_t pa_l1 = s->header.pa_l1 + (id_l1 * SELFIE_PAGE_SIZE);
    const int rw = image_pwrite(s, pa_l1, node->l1_page, SELFIE_PAGE_SIZE);
    assert(rw == SELFIE_PAGE_SIZE);
    selfie_log_addr(s, "*L1_WRITE", pa_l1, SELFIE_PAGE_SIZE);
    atomic_inc(&(s->nr_write_l1));
    node->dirty1 = false;
  }
  __unlock(&(node->write_lock));
}

  static void
index_map(struct SelfieState * const s, const uint64_t va, const uint64_t pa, const bool write)
{
  // check aligned
  const uint64_t va_lock_id = (va >> s->header.block_shift) % I_LOCK_SCALE;
  assert((va % s->block_size) == 0);
  assert((pa % SELFIE_PAGE_SIZE) == 0);
  const uint64_t spg = s->header.block_shift;
  const uint64_t id_l1 = (va >> (spg + 18));
  const uint64_t id_l2 = (va >> (spg + 9)) & 0x1ff;
  const uint64_t id_pg = (va >> spg) & 0x1ff;
  assert(id_l1 < s->header.nr_l1);

  struct SelfieIndexL1 * const node = &(s->nodes[id_l1]);
  assert(node->l1_page);
  // alloc if no l2 in memory
  if (node->l2_pages[id_l2] == NULL) { // alloc l2
    uint64_t * const l2_page = aligned_alloc(SELFIE_PAGE_SIZE, SELFIE_PAGE_SIZE);
    assert(l2_page);
    bzero(l2_page, SELFIE_PAGE_SIZE);
    node->l2_pages[id_l2] = l2_page;
  }

  // check if changed (likely)
  if (node->l2_pages[id_l2][id_pg] != pa) {
    node->l2_pages[id_l2][id_pg] = pa;
    node->dirty2[id_l2] = true;
  }

  // write
  if (write) { // write mapping
    index_write_id(s, id_l1, id_l2);
  }
  __unlock(&(s->index_lock[va_lock_id]));
}

  static inline void
index_map_soft(struct SelfieState * const s, const uint64_t va, const uint64_t pa)
{
  index_map(s, va, pa, false);
}

  static inline void
index_map_hard(struct SelfieState * const s, const uint64_t va, const uint64_t pa)
{
  index_map(s, va, pa, true);
}

  static uint64_t
index_translate(struct SelfieState * const s, const uint64_t va)
{
  assert((va % s->block_size) == 0);
  const uint64_t spg = s->header.block_shift;
  const uint64_t id_l1 = (va >> (spg + 18));
  const uint64_t id_l2 = (va >> (spg + 9)) & 0x1ff;
  const uint64_t id_pg = (va >> spg) & 0x1ff;
  if ((id_l1 < s->header.nr_l1) && (s->nodes[id_l1].l2_pages[id_l2])) {
    const uint64_t pa = s->nodes[id_l1].l2_pages[id_l2][id_pg];
    assert((pa % SELFIE_PAGE_SIZE) == 0);
    return pa;
  } else {
    // 0 == no mapping
    return 0;
  }
}

  static void
index_node_free(struct SelfieIndexL1 * const node)
{
  if (node == NULL) return;
  uint64_t i;
  for (i = 0; i < 512; i++) {
    if (node->l2_pages[i]) free(node->l2_pages[i]);
  }
  if (node->l1_page) free(node->l1_page);
  // don't free(node)
}

  static void
index_free(struct SelfieState * const s)
{
  if (s->nodes == NULL) return;
  uint64_t i;
  for (i = 0; i < s->header.nr_l1; i++) {
    index_node_free(&(s->nodes[i]));
  }
  free(s->nodes);
  s->nodes = NULL;
}

  static void
index_mapping_print(struct SelfieState * const s, const char * const tag)
{
  uint64_t cz = 0;
  uint64_t cn = 0;
  uint64_t cx = 0;
  uint64_t va;
  for (va = 0; va < s->header.capacity; va += s->block_size) {
    const uint64_t pa = index_translate(s, va);
    if (pa) {
      switch (zone_pa_type(s, pa)) {
        case ZONE_TYPE_Z: cz++; break;
        case ZONE_TYPE_N: cn++; break;
        default: cx++; break;
      }
      //selfie_log(s, "%s-MAPPING@%016"PRIx64"->%016"PRIx64" %s", tag, va, pa, zone_pa_type_str(s, pa));
    }
  }
  selfie_log(s, "%s mappings: %"PRIu64"Z, %"PRIu64"N, %"PRIu64"?", tag, cz, cn, cx);
}
// }}}
// {{{ read with zpage/mapping
  static void
data_read_decode_z(struct SelfieState * const s, uint8_t * const buf)
{
  uint8_t zp[SELFIE_PAGE_SIZE] __attribute__ ((aligned(SELFIE_PAGE_SIZE)));
  struct SelfieZPage * const zpage = (typeof(zpage))zp;
  memcpy(zp, buf, SELFIE_PAGE_SIZE);
  selfie_log_addr(s, "|--+>R_VA_DECODE_Z", zpage->zh.va, SELFIE_PAGE_SIZE);
  const bool rd = zpage_decode(s, buf, zpage);
  if (rd == false) {
    bzero(buf, s->block_size);
  }
}

// read must success (assertion on illegal parameters)
// transparently decode zpage
// bzero on any exception
// va always aligned to s->block_size
  static void
data_read_va(struct SelfieState * const s, const uint64_t va, uint8_t * const buf)
{
  selfie_log_addr(s, "|-->R_VA", va, s->block_size);
  // mapping va -> pa
  // check aligned va
  assert((va % s->block_size) == 0);
  assert(va < s->header.capacity);
  const uint64_t pa = index_translate(s, va);
  if (pa == 0) {
    bzero(buf, s->block_size);
    return;
  }
  // read from pa
  const int rr = image_pread(s, pa, buf, s->block_size);
  assert(rr == s->block_size);
  // if in z-zone, decompress the head page
  if (zone_pa_type(s, pa) == ZONE_TYPE_Z) {
    data_read_decode_z(s, buf);
  }
}

// }}}
// {{{ write with zpage/mapping
  static void
data_write_alloc_z(struct SelfieState * const s, const uint64_t va, const struct SelfieZPage * const zpage)
{
  const uint64_t pa = zone_alloc_z(s, va);
  selfie_log_addr(s, "|--+>W_AL_Z_PA", pa, s->block_size);
  assert(pa > 0);
  index_map_soft(s, va, pa);
  atomic_inc(&(s->nr_write_data_z));
  const int rw = image_pwrite(s, pa, zpage->buf, s->block_size);
  assert(rw == s->block_size);
}

  static void
data_write_alloc_n(struct SelfieState * const s, const uint64_t va, const uint8_t * const buf)
{
  const uint64_t pa = zone_alloc_n(s, va);
  selfie_log_addr(s, "|--+>W_AL_N_PA", pa, s->block_size);
  assert(pa > 0);
  index_map_hard(s, va, pa);
  atomic_inc(&(s->nr_write_data_n));
  const int rw = image_pwrite(s, pa, buf, s->block_size);
  assert(rw == s->block_size);
}

// do alloc and write aligned page
  static void
data_write_alloc(struct SelfieState * const s, const uint64_t va, const uint8_t * const buf)
{
  selfie_log_addr(s, "|--->W_ALLOC", va, SELFIE_PAGE_SIZE);
  // try compress to z-zone
  assert((va % s->block_size) == 0);
  uint8_t zp[s->zbuffer_size] __attribute__ ((aligned(SELFIE_PAGE_SIZE)));
  struct SelfieZPage * const zpage = (typeof(zpage))zp;
  bzero(zp, SELFIE_PAGE_SIZE);
  const bool rz = zpage_encode(s, buf, zpage, va);
  if (rz == true) {
    // compressible
    if (s->block_size > SELFIE_PAGE_SIZE) {
      memcpy(&(zp[SELFIE_PAGE_SIZE]), &(buf[SELFIE_PAGE_SIZE]), s->block_size - SELFIE_PAGE_SIZE);
    }
    data_write_alloc_z(s, va, zpage);
  } else {
    data_write_alloc_n(s, va, buf);
  }
}

// write aligned whole block (of s->block_size)
  static void
data_write_va(struct SelfieState * const s, const uint64_t va, const uint8_t * const buf)
{
  selfie_log_addr(s, "|-->W_VA", va, s->block_size);
  assert((va % s->block_size) == 0);
  const uint64_t va_lock_id = (va >> s->header.block_shift) % I_LOCK_SCALE;
  // lock index
  __lock(&(s->index_lock[va_lock_id]));
  const uint64_t pa = index_translate(s, va);
  if (pa == 0) { // need alloc
    data_write_alloc(s, va, buf);
    // unlocked in data_write_alloc()
    return;
  }
  // va has mapping
  const uint32_t pa_type = zone_pa_type(s, pa);
  if (pa_type == ZONE_TYPE_Z) { // allocated in z-zone, try to compress
    uint8_t zp[s->zbuffer_size] __attribute__ ((aligned(SELFIE_PAGE_SIZE)));
    bzero(zp, SELFIE_PAGE_SIZE);
    struct SelfieZPage * const zpage = (typeof(zpage))zp;
    const bool rz = zpage_encode(s, buf, zpage, va);
    if (rz == true) { // can compress, write it
      __unlock(&(s->index_lock[va_lock_id]));
      // write without metadata update
      selfie_log_addr(s, "|-+>W_Z_PA", pa, s->block_size);
      atomic_inc(&(s->nr_write_data_z));
      const int rw1 = image_pwrite(s, pa, zpage->buf, SELFIE_PAGE_SIZE);
      assert(rw1 == SELFIE_PAGE_SIZE);
      if (s->block_size > SELFIE_PAGE_SIZE) {
        const int rw2 = image_pwrite(s, pa+SELFIE_PAGE_SIZE, buf+SELFIE_PAGE_SIZE, s->block_size-SELFIE_PAGE_SIZE);
        assert(rw2 == (s->block_size - SELFIE_PAGE_SIZE));
      }
    } else { // cannot compress, alloc n-zone space and write
      // TODO: reclaim the lost z-zone space
      data_write_alloc_n(s, va, buf);
    }
  } else if (pa_type == ZONE_TYPE_N) { // n-zone, just write
    __unlock(&(s->index_lock[va_lock_id]));
    // write without metadata update
    selfie_log_addr(s, "|-+>W_N_PA", pa, s->block_size);
    atomic_inc(&(s->nr_write_data_n));
    const int rw = image_pwrite(s, pa, buf, s->block_size);
    assert(rw == s->block_size);
  } else {
    selfie_log_addr(s, "|-+>ERROR: write to O/L ZONE", pa, s->block_size);
    assert(false);
  }
}

  static void
data_write_va_partial(struct SelfieState * const s, const uint64_t va,
    const uint8_t * const buf, const uint64_t length)
{
  //  (1) read the page if it exists.
  selfie_log_addr(s, "|+>W_PART_VA", va, length);
  const uint64_t shift = s->header.block_shift;
  const uint64_t va_aligned = (va >> shift) << shift;
  const uint64_t pg_off = va - va_aligned;
  assert((pg_off + length) <= s->block_size);
  uint8_t page[s->block_size] __attribute__((aligned (SELFIE_PAGE_SIZE)));
  const uint64_t pa = index_translate(s, va_aligned);
  if (pa == 0) { // fastpath: alloc-write with no read
    bzero(page, s->block_size);
    memcpy(&(page[pg_off]), buf, length);
    data_write_va(s, va_aligned, page);
  } else if (pg_off < SELFIE_PAGE_SIZE) { // need to read anyway
    data_read_va(s, va_aligned, page);
    memcpy(&(page[pg_off]), buf, length);
    data_write_va(s, va_aligned, page);
  } else { // write to pa with no read
    const int rw = image_pwrite(s, pa+pg_off, buf, length);
    assert(rw == length);
  }
}
// }}}
// {{{ selfie open
// this should be called at the end of selfie_open()
// read z-zone index, update in memory only
// only called on open()
// called in non-coroutine

  static void
selfie_open_init_locks(struct SelfieState * const s)
{
  uint64_t x;
  for (x = 0; x < I_LOCK_SCALE; x++) {
    qemu_co_mutex_init(&(s->index_lock[x]));
  }
  qemu_co_mutex_init(&(s->zone_lock));
}

// load all zone metadata from the image
  static void
selfie_open_load_zones(struct SelfieState * const s)
{
  const uint64_t nr_zones = s->header.nr_zones;
  const int zi_size = sizeof(s->zones[0]) * nr_zones;
  s->zones = g_malloc0(zi_size);
  assert(s->zones != NULL);
  selfie_log_addr(s, "*ZONE_LOAD_ALL",s->header.pa_zi, zi_size);
  const int rz = bdrv_pread(s->main, s->header.pa_zi, s->zones, zi_size);
  assert(rz == zi_size);
  int i;
  for (i = 0; i < nr_zones; i++) {
    switch (s->zones[i].t) {
      case ZONE_TYPE_Z: selfie_log(s, "LOAD ZONE[%4d]:Z", i); break;
      case ZONE_TYPE_N: selfie_log(s, "LOAD ZONE[%4d]:N", i); break;
      case ZONE_TYPE_L: selfie_log(s, "LOAD ZONE[%4d]:L", i); break;
      default: break;
    }
  }
}

// initialize id_lzone
  static void
selfie_open_lzones(struct SelfieState * const s)
{
  // scan for empty n-zone
  const uint64_t nr_zones = s->header.nr_zones;
  s->id_lzone = nr_zones + 10;
  uint64_t i;
  for (i = 0; i < nr_zones; i++) {
    if (s->zones[i].t == ZONE_TYPE_0) { // unused zone. alloc new zone
      s->id_lzone = i;
      const bool rn = zone_alloc_type(s, ZONE_TYPE_L);
      assert(rn);
      break;
    }
    if ((s->zones[i].t == ZONE_TYPE_L) && (s->zones[i].n < s->nr_zone_page)) {
      s->id_lzone = i;
      break;
    }
  }
  assert(s->id_lzone < nr_zones);
}

// initialize id_nzone
  static void
selfie_open_nzones(struct SelfieState * const s)
{
  // scan for empty n-zone
  const uint64_t nr_zones = s->header.nr_zones;
  s->id_nzone = nr_zones + 10;
  uint64_t i;
  for (i = 0; i < nr_zones; i++) {
    if (s->zones[i].t == ZONE_TYPE_0) { // unused zone. alloc new zone
      s->id_nzone = i;
      const bool rn = zone_alloc_type(s, ZONE_TYPE_N);
      assert(rn);
      break;
    }
    if ((s->zones[i].t == ZONE_TYPE_N) && (s->zones[i].n < s->nr_zone_unit)) {
      s->id_nzone = i;
      break;
    }
  }
  assert(s->id_nzone < nr_zones);
}

  static void
selfie_open_load_l2(struct SelfieState * const s, struct SelfieIndexL1 * const node, const uint64_t j)
{
  const uint64_t next_pa_n = zone_id_to_pa(s, s->id_nzone, s->zones[s->id_nzone].n);
  const uint64_t pa_l2 = node->l1_page[j];
  // read valid l2
  node->l2_pages[j] = aligned_alloc(SELFIE_PAGE_SIZE, SELFIE_PAGE_SIZE);
  assert(node->l2_pages[j]);
  assert(zone_pa_type(s, pa_l2) == ZONE_TYPE_L);
  const ssize_t r2 = bdrv_pread(s->main, pa_l2, node->l2_pages[j], SELFIE_PAGE_SIZE);
  assert(r2 == SELFIE_PAGE_SIZE);
  uint64_t * const l2_page = node->l2_pages[j];
  uint64_t k;
  for (k = 0; k < 512; k++) {
    const uint64_t pa_data = l2_page[k];
    if (pa_data && (zone_pa_type(s, pa_data) == ZONE_TYPE_N) && (pa_data >= next_pa_n)) {
      l2_page[k] = 0; // invalid pa_data
    }
  }
}

  static void
selfie_open_load_l1(struct SelfieState * const s, const uint64_t i)
{
  struct SelfieIndexL1 * const node = &(s->nodes[i]);
  const uint64_t next_pa_l2 = zone_id_to_pa(s, s->id_lzone, s->zones[s->id_lzone].n);
  // load l1
  node->l1_page = aligned_alloc(SELFIE_PAGE_SIZE, SELFIE_PAGE_SIZE);
  assert(node->l1_page);
  const uint64_t pa_l1 = s->header.pa_l1 + (i * SELFIE_PAGE_SIZE);
  const ssize_t r1 = bdrv_pread(s->main, pa_l1, node->l1_page, SELFIE_PAGE_SIZE);
  assert(r1 == SELFIE_PAGE_SIZE);
  uint64_t j;
  // load l2
  for (j = 0; j < 512; j++) {
    const uint64_t pa_l2 = node->l1_page[j];
    if (pa_l2 == 0) continue;

    if (pa_l2 >= next_pa_l2) {
      // invalid pa_l2
      node->l1_page[j] = 0;
    } else { // pa_l2 is valid
      selfie_open_load_l2(s, node, j);
    }
  }
}

  static void
selfie_open_load_index(struct SelfieState * const s)
{
  // alloc IndexL1
  const uint64_t nr_l1 = s->header.nr_l1;
  const size_t index_nodes_size = sizeof(s->nodes[0]) * nr_l1;
  s->nodes = g_malloc0(index_nodes_size);
  assert(s->nodes);
  // get max valid l2_pa;
  // get max valid n-zone pa;

  uint64_t i;
  for (i = 0; i < nr_l1; i++) {
    qemu_co_mutex_init(&(s->nodes[i].write_lock));
    selfie_open_load_l1(s, i);
  }
}

  static void
open_scan_zzone(struct SelfieState * const s, const uint64_t id)
{
  assert(s->zones[id].n == 0); // scan a 0 z-zone
  selfie_log(s, "scanning z [%"PRIu64"]", id);
  uint8_t buf[s->block_size] __attribute__((aligned(SELFIE_PAGE_SIZE)));
  uint8_t zp[s->block_size] __attribute__((aligned(SELFIE_PAGE_SIZE)));
  struct SelfieZPage * const zpage = (typeof(zpage))zp;
  assert(zpage);

  uint64_t i;
  for (i = 0; i < s->nr_zone_unit; i++) {
    const uint64_t pa = zone_id_to_pa(s, id, i);
    const int rr = bdrv_pread(s->main, pa, zpage->buf, s->block_size);
    assert(rr == s->block_size);
    // check
    const bool rd = zpage_decode(s, buf, zpage);
    if (rd == true) {
      s->zones[id].n++;
      const uint64_t npa = index_translate(s, zpage->zh.va);
      if (npa == 0) {
        const uint64_t va_lock_id = (zpage->zh.va >> s->header.block_shift) % I_LOCK_SCALE;
        __lock(&(s->index_lock[va_lock_id]));
        index_map_soft(s, zpage->zh.va, pa);
        selfie_log_addr(s, "found zpage", zpage->zh.va, SELFIE_PAGE_SIZE);
      } else {
        // If map exists, the z-page has been replaced by a n-page, or has been written.
        assert((zone_pa_type(s, npa) == ZONE_TYPE_N) || (npa == pa));
        // if it's a n-page the zpage is be invalid and the space should be reclaimed.
        // TODO: reclaim leaked z-zone space
      }
    } else {
      // no more z-page, finish.
      break;
    }
  }
  selfie_log(s, "scanned, found %"PRIu64" pages, max %"PRIu64, s->zones[id].n, s->nr_zone_unit);
}

  static void
selfie_open_scan_zzones(struct SelfieState * const s)
{
  const uint64_t nr_zones = s->header.nr_zones;
  s->id_zzone = nr_zones + 10; // invalid id
  // scan for last z-zone
  uint64_t i;
  for (i = 0; i < nr_zones; i++) {
    if (s->zones[i].t == ZONE_TYPE_0) { // unused zone. mark as a z-zone
      s->id_zzone = i;
      const bool rz = zone_alloc_type(s, ZONE_TYPE_Z);
      assert(rz);
      // no more scan
      break;
    }
    if (s->zones[i].t == ZONE_TYPE_Z) { // zzone
      if (s->zones[i].n == 0) { // mapping not synced
        s->id_zzone = i;
        open_scan_zzone(s, i);
        if (s->zones[i].n != s->nr_zone_unit) {
          // found an half-used zzone
          break;
        }
      } else {
        // a z-zone must be either 0 or full
        assert(s->zones[i].n == s->nr_zone_unit);
        // continue on next id
      }
    }
  }
  assert(s->id_zzone < nr_zones);
}

  static int
selfie_open(BlockDriverState * const bs, QDict *options, int flags, Error **errp)
{
  struct SelfieState * const s = bs->opaque;
  bzero(s, sizeof(*s));
  // read header
  const int rh = bdrv_pread(bs->file, 0, &(s->header), sizeof(s->header));
  assert(rh == sizeof(s->header));
  // for debug
  s->fd_log = open("/tmp/selfie-debug.log", O_CREAT | O_APPEND | O_WRONLY | O_DSYNC, 00644);
  selfie_log(s, "capacity: %"PRIu64, s->header.capacity);
  selfie_log(s, "shift: %"PRIu64, s->header.block_shift);
  selfie_log(s, "nr_l1: %"PRIu64, s->header.nr_l1);
  selfie_log(s, "zone_size: %"PRIu64, s->header.zone_size);
  selfie_log(s, "nr_zones: %"PRIu64, s->header.nr_zones);
  selfie_log(s, "pa_zi: %"PRIu64, s->header.pa_zi);
  selfie_log(s, "pa_l1: %"PRIu64, s->header.pa_l1);
  selfie_log(s, "pa_zones: %"PRIu64, s->header.pa_zones);
  selfie_log(s, "init_type: %"PRIu64, s->header.init_type);
  s->main = bs->file;
  // setup bs
  s->block_size = 1 << s->header.block_shift;
  s->zdata_size = SELFIE_PAGE_SIZE - sizeof(struct SelfiePageHead);
  const uint64_t bound = LZ4_compressBound(s->block_size);
  s->zbuffer_size = s->block_size;
  while (s->zbuffer_size < bound) s->zbuffer_size += SELFIE_PAGE_SIZE;
  s->nr_zone_unit = s->header.zone_size / s->block_size;
  s->nr_zone_page = s->header.zone_size / SELFIE_PAGE_SIZE;
  bs->total_sectors = s->header.capacity / 512;
  // load zone metadata
  selfie_open_init_locks(s);
  selfie_open_load_zones(s);
  selfie_open_lzones(s);
  selfie_open_nzones(s);
  selfie_open_load_index(s);
  selfie_open_scan_zzones(s);
  index_mapping_print(s, "OPEN");
  return 0;
}

// }}}
// {{{ selfie_read API
  static int
selfie_read(struct SelfieState * const s, const uint64_t sector_num,
    uint8_t * const buf, const uint64_t nb_sectors)
{
  // LOG
  selfie_log_addr(s, "|->SELFIE_READ", sector_num * 512, nb_sectors * 512);
  if (nb_sectors == 0) return 0;

  uint64_t i;
  uint8_t page[s->block_size] __attribute__((aligned (SELFIE_PAGE_SIZE)));
  const uint64_t shift = s->header.block_shift;
  uint64_t cur_pva = (sector_num * 512) >> shift << shift;
  // read first block
  data_read_va(s, cur_pva, page);
  for (i = 0; i < nb_sectors; i++) {
    const uint64_t sva = ((sector_num + i) * 512) >> shift << shift;
    if (sva != cur_pva) {
      // read
      data_read_va(s, sva, page);
      cur_pva = sva;
    }
    const uint64_t poff = ((sector_num + i) * 512) % s->block_size;
    const uint64_t boff = i * 512;
    memcpy(&(buf[boff]), &(page[poff]), 512);
  }
  return 0;
}

  static int coroutine_fn
selfie_co_read(BlockDriverState * const bs, const int64_t sector_num,
    const int nb_sectors, QEMUIOVector *qiov)
{
  struct SelfieState * const s = bs->opaque;
  assert((nb_sectors*512) == qiov->size);
  if ((sector_num + nb_sectors) * 512 > s->header.capacity) {
    selfie_log(s, "ERROR: read beyond capacity");
    return -EINVAL;
  }
  int i;
  int64_t sec_iter = sector_num;
  for (i = 0; i < qiov->niov; i++) {
    const size_t nr_sec = qiov->iov[i].iov_len>>9;
    selfie_read(s, sec_iter, qiov->iov[i].iov_base, nr_sec);
    sec_iter += nr_sec;
  }
  return 0;
}
// }}}
// {{{ selfie_write API

  static int
selfie_write(struct SelfieState * const s, const int64_t sector_num,
    const uint8_t * const buf, const uint64_t nb_sectors)
{
  const uint64_t off_start = sector_num * UINT64_C(512);
  const uint64_t off_end = (sector_num + nb_sectors) * UINT64_C(512);
  uint64_t va_page;
  // LOG
  const uint64_t shift = s->header.block_shift;
  selfie_log_addr(s, "|->SELFIE_WRITE", off_start, off_end - off_start);
  for (va_page = ((off_start >> shift) << shift); va_page < off_end; va_page += s->block_size) {
    const uint64_t va0 = (va_page < off_start) ? off_start : va_page;
    const uint64_t va1 = ((va_page + s->block_size) < off_end) ? (va_page + s->block_size) : off_end;
    const uint64_t offset = va0 - off_start;
    const uint64_t length = va1 - va0;
    if (length < s->block_size) { // what ever
      data_write_va_partial(s, va0, &(buf[offset]), length);
    } else { // whole block write
      data_write_va(s, va0, &(buf[offset]));
    }
  }
  return 0;
}

  static int coroutine_fn
selfie_co_write(BlockDriverState * const bs, const int64_t sector_num,
    const int nb_sectors, QEMUIOVector *qiov)
{
  if (bs->read_only)
    return -EACCES;
  struct SelfieState * const s = bs->opaque;
  assert((nb_sectors * 512) == qiov->size);
  if ((sector_num + nb_sectors) * 512 > s->header.capacity) {
    selfie_log(s, "fatal error@selfie_co_write(): write after END");
    return -EINVAL;
  }
  int i;
  int64_t sec_iter = sector_num;
  for (i = 0; i < qiov->niov; i++) {
    if (qiov->iov[i].iov_len % 512) {
      selfie_log(s, "fatal error@selfie_co_write(): iov not aligned!");
    }
    const size_t nr_sec = qiov->iov[i].iov_len>>9;
    selfie_write(s, sec_iter, qiov->iov[i].iov_base, nr_sec);
    sec_iter += nr_sec;
  }
  return 0;
}
// }}}
// {{{ selfie_create API
  static int
selfie_create(const char *filename, QemuOpts *opts, Error **errp)
{
  const int fd_log = open("/tmp/selfie-debug.log", O_CREAT | O_APPEND | O_WRONLY | O_DSYNC, 00644);
  // create and open image file
  Error *local_err = NULL;
  const int rc = bdrv_create_file(filename, NULL, &local_err);
  if (rc < 0) {
    error_propagate(errp, local_err);
    return rc;
  }

  BlockDriverState * bs = NULL;
  int ro = bdrv_open(&bs, filename, NULL, NULL, BDRV_O_RDWR | BDRV_O_PROTOCOL, NULL, &local_err);
  if (ro < 0) {
    error_propagate(errp, local_err);
    return ro;
  }

  // key parameters
  const uint64_t capacity = qemu_opt_get_size_del(opts, BLOCK_OPT_SIZE, 1024*1024);
  const uint64_t cluster_size = qemu_opt_get_size_del(opts, BLOCK_OPT_CLUSTER_SIZE, 4 * 1024);
  const uint64_t zone_size = qemu_opt_get_size_del(opts, "zone_size", 4*1024*1024);
  char * const init_opt = qemu_opt_get_del(opts, "init");
  dprintf(fd_log, "capacity: %"PRIu64"\n", capacity);
  dprintf(fd_log, "cluster_size: %"PRIu64"\n", cluster_size);
  dprintf(fd_log, "zone_size: %"PRIu64"\n", zone_size);
  dprintf(fd_log, "init_opt: %s\n", init_opt?init_opt:"");

  if (cluster_size < SELFIE_PAGE_SIZE) return -EINVAL; // >= 4K
  if (cluster_size & (cluster_size - 1)) return -EINVAL; // must be 2^x
  if (zone_size < cluster_size) return -EINVAL;
  if (zone_size & (zone_size - 1)) return -EINVAL; // must be 2^x
  if (capacity == 0) return -EINVAL; // non zero
  if ((capacity % cluster_size) != 0) return -EINVAL; // multiple of cluster_size

  // prepare header
  struct SelfieHeader zh;
  memcpy(zh.magic, SELFIE_MAGIC, sizeof(SELFIE_MAGIC));
  // ->capacity
  zh.capacity = capacity;
  // ->block_shift
  uint64_t sh = 12;
  while ((1 << sh) < cluster_size) sh++;
  assert((1 << sh) == cluster_size);
  zh.block_shift = sh;
  // ->nr_l1
  const uint64_t size_l1 = cluster_size * 512 * 512;
  uint64_t nr_l1 = 1;
  while ((nr_l1 * size_l1) < capacity) nr_l1++;
  zh.nr_l1 = nr_l1;
  // ->zone_size
  zh.zone_size = zone_size;
  // ->nr_zones
  zh.nr_zones = (capacity / zone_size) * 2 + 1;
  const uint64_t zone_pages = (zh.nr_zones * sizeof(struct SelfieZoneInfo)) / SELFIE_PAGE_SIZE + 1;
  // ->pa_zi
  zh.pa_zi = SELFIE_PAGE_SIZE;
  // ->pa_l1
  zh.pa_l1 = SELFIE_PAGE_SIZE * (zone_pages + 1); // first page after header
  // ->pa_zones
  zh.pa_zones = SELFIE_PAGE_SIZE * (zone_pages + nr_l1 + 1);
  // ->init_type
  uint64_t init_type = INIT_ZERO; // default
  if (init_opt) {
    if (strcmp(init_opt, "trim") == 0) {
      init_type = INIT_TRIM;
    } else if (strcmp(init_opt, "none") == 0) {
      init_type = INIT_NONE;
    } // otherwise -> none
  }
  zh.init_type = init_type;
  // write header
  const int rh = bdrv_pwrite(bs, 0, &zh, sizeof(zh));
  if (rh != sizeof(zh)) return rh;
  dprintf(fd_log, "capacity: %"PRIu64"\n", zh.capacity);
  dprintf(fd_log, "shift: %"PRIu64"\n", zh.block_shift);
  dprintf(fd_log, "nr_l1: %"PRIu64"\n", zh.nr_l1);
  dprintf(fd_log, "zone_size: %"PRIu64"\n", zh.zone_size);
  dprintf(fd_log, "nr_zones: %"PRIu64"\n", zh.nr_zones);
  dprintf(fd_log, "pa_zi: %"PRIu64"\n", zh.pa_zi);
  dprintf(fd_log, "pa_l1: %"PRIu64"\n", zh.pa_l1);
  dprintf(fd_log, "pa_zones: %"PRIu64"\n", zh.pa_zones);
  dprintf(fd_log, "init_type: %"PRIu64"\n", zh.init_type);

  // write zoneinfo and l1 (zeroes)
  const uint64_t zeroes_size = (zone_pages + nr_l1) * SELFIE_PAGE_SIZE;
  void * const zeroes = g_malloc0(zeroes_size);
  bdrv_pwrite(bs, zh.pa_zi, zeroes, zeroes_size);
  free(zeroes);
  // close
  bdrv_unref(bs);
  close(fd_log);
  return 0;
}
// }}}
// {{{ misc. API
  static int
selfie_get_info(BlockDriverState * const bs, BlockDriverInfo * const bdi)
{
  struct SelfieState * const s = bs->opaque;
  bdi->cluster_size = s->block_size;
  bdi->vm_state_offset = 0;
  bdi->unallocated_blocks_are_zero = true;
  bdi->can_write_zeroes_with_unmap = false; // ?
  bdi->needs_compressed_writes = false;
  return 0;
}

  static int
selfie_probe(const uint8_t * const buf, const int buf_size, const char *filename)
{
  (void)filename;
  if (buf_size < 8) return 0;
  if (memcmp(buf, SELFIE_MAGIC, 8) == 0) return 100;
  else return 0;
}

  static void
selfie_close(BlockDriverState * const bs)
{
  struct SelfieState * const s = bs->opaque;
  // print stat
  index_mapping_print(s, "CLOSE");
  selfie_log(s, "W_Z %"PRIu64" W_N %"PRIu64" W_ZONE %"PRIu64" W_L1 %"PRIu64" W_L2 %"PRIu64,
      s->nr_write_data_z, s->nr_write_data_n, s->nr_write_zone, s->nr_write_l1, s->nr_write_l2);
  index_free(s);
  selfie_log(s, "CLOSE: index freed");
  free(s->zones);
  selfie_log(s, "CLOSE: zones freed");
  //close
  selfie_log(s, "#### closed ####");
  close(s->fd_log);
}

  static int64_t
selfie_get_allocated_file_size(BlockDriverState * const bs)
{
  return bdrv_get_allocated_file_size(bs->file);
}

// }}}
// {{{ BlockDriver
static QemuOptsList selfie_create_opts = {
  .name = "selfie-create-opts",
  .head = QTAILQ_HEAD_INITIALIZER(selfie_create_opts.head),
  .desc = {
    {
      .name = BLOCK_OPT_SIZE,
      .type = QEMU_OPT_SIZE,
      .help = "Virtual disk size",
    },
    {
      .name = BLOCK_OPT_CLUSTER_SIZE,
      .type = QEMU_OPT_SIZE,
      .help = "Cluster size (default 4KB)",
      .def_value_str = stringify(4096),  // 4KB
    },
    {
      .name = "zone_size",
      .type = QEMU_OPT_SIZE,
      .help = "Zone size (default 16MB)",
    },
    {
      .name = "init",
      .type = QEMU_OPT_STRING,
      .help = "Initialize with {trim|zero|none}",
    },
    { /* end of list */ }
  }
};

static BlockDriver bdrv_selfie = {
  .format_name = "selfie",
  .instance_size = sizeof(struct SelfieState),
  .bdrv_get_info = selfie_get_info,
  .bdrv_probe = selfie_probe,
  .bdrv_open   = selfie_open,
  .bdrv_create = selfie_create,
  .bdrv_co_readv   = selfie_co_read,
  .bdrv_co_writev  = selfie_co_write,
  .bdrv_close  = selfie_close,
  .bdrv_get_allocated_file_size = selfie_get_allocated_file_size,
  //.bdrv_co_flush_to_disk = selfie_co_flush_to_disk,

  .bdrv_has_zero_init = bdrv_has_zero_init_1,
  .create_opts = &selfie_create_opts,
};

static void bdrv_selfie_init(void)
{
  bdrv_register(&bdrv_selfie);
}

block_init(bdrv_selfie_init);
// }}}
// vim: fdm=marker
