// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 37  // or 41/53; prime near NBUF to reduce collisions

static inline uint
mix32(uint x) {
  // A tiny 32-bit mixer (public-domain style); good enough for blockno/dev.
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

static inline int
bucket_idx(uint dev, uint blockno)
{
  // Mix blockno strongly, and fold-in dev with a different constant.
  uint h = mix32(blockno) ^ (mix32(dev) * 0x9e3779b1u);
  return (int)(h % NBUCKETS);
}

// Doubly-linked list helpers (circular list with a sentinel head).
static inline void
list_remove(struct buf *b)
{
  b->next->prev = b->prev;
  b->prev->next = b->next;
}

static inline void
list_insert_head(struct buf *head, struct buf *b)
{
  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
}

struct {
  // Keep a global lock only for stats/debug if you want; not used in fast path.
  struct spinlock lock;  // name "bcache", kept mostly idle
  // Per-bucket spinlocks and heads (each head is a sentinel of a circular list).
  struct spinlock bucket_locks[NBUCKETS];
  struct buf bucket_heads[NBUCKETS];
  // All buffers storage stays the same; we do not change NBUF nor count.
  struct buf buf[NBUF];
} bcache;

// Optional: unique names per bucket are not required; prefix must be "bcache".
static char bucket_lock_name[] = "bcache.bucket";

void
binit(void)
{
  initlock(&bcache.lock, "bcache");

  // Initialize bucket locks and heads.
  for (int i = 0; i < NBUCKETS; i++) {
    initlock(&bcache.bucket_locks[i], bucket_lock_name);
    bcache.bucket_heads[i].next = &bcache.bucket_heads[i];
    bcache.bucket_heads[i].prev = &bcache.bucket_heads[i];
  }

  // Initialize buffers and distribute them round-robin into buckets.
  // We do this without locking since binit runs at boot before concurrency.
  int idx = 0;
  for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->valid = 0;
    b->disk = 0;
    b->dev = 0;
    b->blockno = 0;
    b->refcnt = 0;
    initsleeplock(&b->lock, "buffer");

    // Insert into bucket idx as MRU.
    struct buf *head = &bcache.bucket_heads[idx];
    b->next = head->next;
    b->prev = head;
    head->next->prev = b;
    head->next = b;

    idx = (idx + 1) % NBUCKETS;
  }
}

// Look for a buf for (dev, blockno). If miss, allocate/move one.
// Return with b->lock (sleeplock) held.
static struct buf*
bget(uint dev, uint blockno)
{
  int bi = bucket_idx(dev, blockno);
  struct buf *b;

  // Fast path: lookup in destination bucket only.
  acquire(&bcache.bucket_locks[bi]);
  for (b = bcache.bucket_heads[bi].next; b != &bcache.bucket_heads[bi]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket_locks[bi]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_locks[bi]);

  // Slow path: serialize miss handling to avoid duplicate inserts.
  acquire(&bcache.lock);

  // Re-check under destination bucket while holding the global lock.
  acquire(&bcache.bucket_locks[bi]);
  for (b = bcache.bucket_heads[bi].next; b != &bcache.bucket_heads[bi]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket_locks[bi]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Find a free (refcnt==0) buffer: try destination bucket first...
  struct buf *victim = 0;
  for (b = bcache.bucket_heads[bi].prev; b != &bcache.bucket_heads[bi]; b = b->prev) {
    if (b->refcnt == 0) {
      // unlink from destination bucket
      b->prev->next = b->next;
      b->next->prev = b->prev;
      victim = b;
      break;
    }
  }
  release(&bcache.bucket_locks[bi]);

  // ...then scan other buckets, holding only one bucket lock at a time.
  if (!victim) {
    for (int i = 0; i < NBUCKETS; i++) {
      if (i == bi) continue;
      acquire(&bcache.bucket_locks[i]);
      for (b = bcache.bucket_heads[i].prev; b != &bcache.bucket_heads[i]; b = b->prev) {
        if (b->refcnt == 0) {
          // unlink from bucket i
          b->prev->next = b->next;
          b->next->prev = b->prev;
          victim = b;
          break;
        }
      }
      release(&bcache.bucket_locks[i]);
      if (victim) break;
    }
  }

  if (!victim) {
    release(&bcache.lock);
    panic("bget: no buffers");
  }

  // Insert victim into destination bucket as MRU and retag it.
  acquire(&bcache.bucket_locks[bi]);
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  victim->next = bcache.bucket_heads[bi].next;
  victim->prev = &bcache.bucket_heads[bi];
  bcache.bucket_heads[bi].next->prev = victim;
  bcache.bucket_heads[bi].next = victim;
  release(&bcache.bucket_locks[bi]);

  release(&bcache.lock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk. Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to MRU position in its own bucket if no one is using it.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bi = bucket_idx(b->dev, b->blockno);
  acquire(&bcache.bucket_locks[bi]);
  if (b->refcnt < 1)
    panic("brelse: refcnt underflow");
  b->refcnt--;
  if (b->refcnt == 0) {
    // Move to MRU inside current bucket.
    list_remove(b);
    list_insert_head(&bcache.bucket_heads[bi], b);
  }
  release(&bcache.bucket_locks[bi]);
}

void
bpin(struct buf *b)
{
  int bi = bucket_idx(b->dev, b->blockno);
  acquire(&bcache.bucket_locks[bi]);
  b->refcnt++;
  release(&bcache.bucket_locks[bi]);
}

void
bunpin(struct buf *b)
{
  int bi = bucket_idx(b->dev, b->blockno);
  acquire(&bcache.bucket_locks[bi]);
  b->refcnt--;
  release(&bcache.bucket_locks[bi]);
}


