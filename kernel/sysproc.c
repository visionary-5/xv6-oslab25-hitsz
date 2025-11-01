#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &flags) < 0) return -1;
  return wait(p,flags);        //获取第二个参数flags 并传给wait
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void)
{
  struct proc *p = myproc();

  // 打印当前进程的内核线程上下文保存区域地址范围
  printf("Save the context of the process to the memory region from address %p to %p\n",
         &p->context, &p->context + 1);

  // 打印当前进程的 pid 和用户态 pc（陷入内核的 ecall 指令地址）
  printf("Current running process pid is %d and user pc is %p\n",
         p->pid, (void*)p->trapframe->epc);

  // 从当前进程起，环形遍历全局进程表，查找下一个 RUNNABLE 进程并打印
  int pi = -1;
  for (int i = 0; i < NPROC; i++) {
    if (&proc[i] == p) { pi = i; break; } // 当前进程 p 在进程表 proc[] 数组中的下标
  }
  
  if (pi != -1) {
    for (int step = 1; step <= NPROC; step++) {
      int idx = (pi + step) % NPROC;
      struct proc *q = &proc[idx];
      if (q == p) continue;

      acquire(&q->lock);
      if (q->state == RUNNABLE) {
        // RUNNABLE 进程的 trapframe->epc 即为它恢复到用户态时的 PC
        printf("Next runnable process pid is %d and user pc is %p\n",
               q->pid, (void*)q->trapframe->epc);
        release(&q->lock);
        break;
      }
      release(&q->lock);
    }
  }

  // 让出 CPU（xv6 已实现）
  yield();
  return 0;
}
