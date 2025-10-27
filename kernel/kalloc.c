// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

// Per-CPU free lists to reduce lock contention
static struct kmem kmems[NCPU];

// persistent lock names; must start with "kmem" for stats aggregation.
static char kmem_lock_names[NCPU][16];

// Push a page onto a specific CPU's freelist.
static void kfree_to_cpu(void *pa, int cid);

void
kinit()
{
  // Initialize per-CPU locks. Names start with "kmem".
  for (int i = 0; i < NCPU; i++) {
    // Build "kmem" or "kmemX" (X for i<10). Always starts with "kmem".
    kmem_lock_names[i][0] = 'k';
    kmem_lock_names[i][1] = 'm';
    kmem_lock_names[i][2] = 'e';
    kmem_lock_names[i][3] = 'm';
    if (i < 10) {
      kmem_lock_names[i][4] = '0' + i;
      kmem_lock_names[i][5] = 0;
    } else {
      // If i >= 10, keep it just "kmem" (still satisfies the prefix requirement).
      kmem_lock_names[i][4] = 0;
    }
    initlock(&kmems[i].lock, kmem_lock_names[i]);
    kmems[i].freelist = 0;
  }

  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  int cpu = 0;
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    kfree_to_cpu(p,cpu);
    cpu = (cpu + 1) % NCPU;      //round-robin distribution
  }
    
}

// Internal helper: free a page to a specific CPU's freelist.
// Used by both freerange (to seed lists) and kfree (for current CPU).
static void
kfree_to_cpu(void *pa, int cid)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmems[cid].lock);
  r->next = kmems[cid].freelist;
  kmems[cid].freelist = r;
  release(&kmems[cid].lock);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
   // cpuid() must be used with interrupts disabled.
  push_off();
  int id = cpuid();
  kfree_to_cpu(pa, id);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r = 0;

  // Keep interrupts off while we identify the current CPU and
  // attempt the local pop; we also keep them off during stealing
  // to keep our CPU id stable.
  push_off();
  int id = cpuid();

  // Try local CPU first.
  acquire(&kmems[id].lock);
  r = kmems[id].freelist;
  if (r)
    kmems[id].freelist = r->next;
  release(&kmems[id].lock);

  // Steal from other CPUs if local list is empty.
  if (r == 0) {
    for (int off = 1; off < NCPU; off++) {
      int i = (id + off) % NCPU;
      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if (r) {
        kmems[i].freelist = r->next;
        release(&kmems[i].lock);
        break;
      }
      release(&kmems[i].lock);
    }
  }

  pop_off();

  if (r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
