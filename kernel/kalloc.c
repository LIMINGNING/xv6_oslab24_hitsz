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

struct kmem kmems[NCPU];

// 静态存储锁名，避免在kinit函数返回后被释放
static char kmem_names[NCPU][16];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    snprintf(kmem_names[i], sizeof(kmem_names[i]), "kmem_%d", i);
    initlock(&kmems[i].lock, kmem_names[i]);
  }

  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 关闭中断，获取当前CPU编号，将页面放入当前CPU的空闲链表
  push_off();
  int cpu = cpuid();
  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  
  // 关闭中断，获取当前CPU编号
  push_off();
  int cpu = cpuid();

  // 首先尝试从当前CPU的空闲链表分配
  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if(r)
    kmems[cpu].freelist = r->next;
  release(&kmems[cpu].lock);

  // 如果当前CPU的空闲链表为空，尝试从其他CPU窃取
  if(!r) {
    for(int i = 0; i < NCPU; i++) {
      if(i == cpu) continue; // 跳过当前CPU
      
      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if(r) {
        kmems[i].freelist = r->next;
        release(&kmems[i].lock);
        break; // 窃取成功，跳出循环
      }
      release(&kmems[i].lock);
    }
  }
  
  pop_off(); // 恢复中断

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
