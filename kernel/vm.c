#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  // 使用 copyin_new 替代原来的软件模拟地址翻译
  // 设置 SSTATUS_SUM 位以允许在 S 模式访问用户页面
  w_sstatus(r_sstatus() | SSTATUS_SUM);
  int ret = copyin_new(pagetable, dst, srcva, len);
  // 清除 SSTATUS_SUM 位
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);
  return ret;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  // 使用 copyinstr_new 替代原来的软件模拟地址翻译
  // 设置 SSTATUS_SUM 位以允许在 S 模式访问用户页面
  w_sstatus(r_sstatus() | SSTATUS_SUM);
  int ret = copyinstr_new(pagetable, dst, srcva, max);
  // 清除 SSTATUS_SUM 位
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);
  return ret;
}

// check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}


// Print "||" indentation: depth times, separated by three spaces.
static void
pt_print_indent(int depth)
{
  for (int i = 0; i < depth; i++) {
    printf("||");
    if (i != depth - 1)
      printf("   ");
  }
}

// Build "rwxu" style flags string for printing with %s.
static void
pt_flags_str(pte_t pte, char out[5])
{
  out[0] = (pte & PTE_R) ? 'r' : '-';
  out[1] = (pte & PTE_W) ? 'w' : '-';
  out[2] = (pte & PTE_X) ? 'x' : '-';
  out[3] = (pte & PTE_U) ? 'u' : '-';
  out[4] = 0;
}

// level: 2=top (VPN[2]), 1=middle (VPN[1]), 0=leaf level (VPN[0])
// va_base accumulates chosen VPN bits so far.
static void
vmprint_rec(pagetable_t pt, int level, uint64 va_base, int depth)
{
  for (int idx = 0; idx < 512; idx++) {
    pte_t pte = pt[idx];
    if ((pte & PTE_V) == 0)
      continue; // only print valid entries

    uint64 pa = PTE2PA(pte);
    char flags[5];
    pt_flags_str(pte, flags);

    // Compute the VA base for this entry index at this level.
    int shift = level * 9 + 12; // 30, 21, 12 for levels 2,1,0
    uint64 va_here = va_base | ((uint64)idx << shift);

    // Non-leaf if V set and no R/W/X set.
    int is_leaf = (pte & (PTE_R | PTE_W | PTE_X)) != 0;

    pt_print_indent(depth);
    if (is_leaf) {
      // Leaf: print VA -> PA with flags
      printf("idx: %d: va: %p -> pa: %p, flags: %s\n",
             idx, (void*)va_here, (void*)pa, flags);
    } else {
      // Non-leaf: print PA (child page table) with flags (likely "----")
      printf("idx: %d: pa: %p, flags: %s\n",
             idx, (void*)pa, flags);

      if (level > 0) {
        // Recurse into child page table
        vmprint_rec((pagetable_t)pa, level - 1, va_here, depth + 1);
      }
    }
  }
}

void
vmprint(pagetable_t pgtbl)
{
  // First line prints the page table pointer itself.
  printf("page table %p\n", pgtbl);
  vmprint_rec(pgtbl, 2, 0, 1);
}

// 创建独立内核页表：保持内核直接映射（不映射 CLINT）
pagetable_t
kvmcreate(void)
{
  pagetable_t kpgtbl = (pagetable_t)kalloc();
  if (kpgtbl == 0)
    return 0;
  memset(kpgtbl, 0, PGSIZE);

  // 设备映射（直接映射）
  if (mappages(kpgtbl, UART0,   PGSIZE,   UART0,   PTE_R|PTE_W) < 0) goto bad;
  if (mappages(kpgtbl, VIRTIO0, PGSIZE,   VIRTIO0, PTE_R|PTE_W) < 0) goto bad;
  if (mappages(kpgtbl, PLIC,    0x400000, PLIC,    PTE_R|PTE_W) < 0) goto bad;

  // 内核文本（只读+可执行）
  if (mappages(kpgtbl, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R|PTE_X) < 0) goto bad;

  // 内核数据 + 剩余物理内存（读写）
  if (mappages(kpgtbl, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R|PTE_W) < 0) goto bad;

  // trampoline（最高页）
  if (mappages(kpgtbl, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R|PTE_X) < 0) goto bad;

  // 注意：不要映射 CLINT
  return kpgtbl;

bad:
  kvmfree(kpgtbl); // 只释放页表页，不释放叶子物理页帧
  return 0;
}

// 递归释放页表页，叶子仅清 PTE，不 free 物理页帧
static void
freewalk_all(pagetable_t pagetable)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) == 0) continue;

    if ((pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      uint64 child = PTE2PA(pte);
      freewalk_all((pagetable_t)child);
      pagetable[i] = 0;
    } else {
      pagetable[i] = 0; // 叶子：只把 PTE 清零
    }
  }
  kfree((void*)pagetable);
}

void
kvmfree(pagetable_t kpgtbl)
{
  if (kpgtbl == 0)
    return;
  
  // 在释放页表前，先清除用户空间的映射（共享的部分）
  // 避免重复释放用户页表的 Level-0 页表
  pte_t *kpte_l2 = &kpgtbl[0];
  if (*kpte_l2 & PTE_V) {
    pagetable_t kpt_l1 = (pagetable_t)PTE2PA(*kpte_l2);
    // 清空用户空间映射范围的 Level-1 PTE（前 96 项）
    // 这些项指向的是用户页表的 Level-0 页表，不应该被释放
    for (int i = 0; i < 96; i++) {
      kpt_l1[i] = 0;
    }
  }
  
  // 现在可以安全地释放页表了
  freewalk_all(kpgtbl);
}

// 同步用户页表到内核页表
// 用户地址空间为 0x0 - 0xC000000 (PLIC地址)
// 按照推荐方案：内核页表的次级页表项直接指向用户页表的叶子页表
void
sync_pagetable(pagetable_t kpgtbl, pagetable_t upgtbl)
{
  // PLIC 地址是 0x0C000000 = 192MB
  // 用户地址空间最大为 PLIC (0 - 192MB)
  // 
  // RISC-V Sv39 三级页表：
  // - Level 2 (顶级): 每项覆盖 1GB (2^30)
  // - Level 1 (次级): 每项覆盖 2MB (2^21)
  // - Level 0 (叶子): 每项覆盖 4KB (2^12)
  //
  // 0xC000000 = 192MB，需要 192/2 = 96 个 Level-1 页表项
  // 这 96 个项都在 Level-2 的第 0 项下面
  
  // 首先，确保内核页表的 Level-2 第0项存在
  pte_t *kpte_l2 = &kpgtbl[0];
  pagetable_t kpt_l1;
  
  if (*kpte_l2 & PTE_V) {
    // 已经存在 Level-1 页表，清空前 96 项（用户空间范围）
    kpt_l1 = (pagetable_t)PTE2PA(*kpte_l2);
    // 清空用户空间映射范围的 Level-1 PTE
    for (int i = 0; i < 96; i++) {
      kpt_l1[i] = 0;
    }
  } else {
    // 创建新的 Level-1 页表
    kpt_l1 = (pagetable_t)kalloc();
    if (kpt_l1 == 0)
      return;
    memset(kpt_l1, 0, PGSIZE);
    *kpte_l2 = PA2PTE(kpt_l1) | PTE_V;
  }
  
  // 获取用户页表的 Level-2 第0项
  pte_t *upte_l2 = &upgtbl[0];
  if (!(*upte_l2 & PTE_V))
    return;  // 用户页表为空
  
  pagetable_t upt_l1 = (pagetable_t)PTE2PA(*upte_l2);
  
  // 复制用户页表 Level-1 的前 96 项到内核页表
  // 96 项 * 2MB = 192MB，正好覆盖 0 - PLIC
  // 注意：这里直接让内核 Level-1 的项指向用户 Level-0 页表（共享）
  for (int i = 0; i < 96; i++) {
    if (upt_l1[i] & PTE_V) {
      kpt_l1[i] = upt_l1[i];
    }
  }
}
