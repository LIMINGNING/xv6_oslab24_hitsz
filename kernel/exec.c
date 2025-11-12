#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);
static void vmprint_walk(pagetable_t pagetable, int level, uint64 va_base, int depth);

int exec(char *path, char **argv) {
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG + 1], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
  if (elf.magic != ELF_MAGIC) goto bad;

  if ((pagetable = proc_pagetable(p)) == 0) goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
    if (ph.type != ELF_PROG_LOAD) continue;
    if (ph.memsz < ph.filesz) goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0) goto bad;
    sz = sz1;
    if (ph.vaddr % PGSIZE != 0) goto bad;
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE)) == 0) goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - 2 * PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG) goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;  // riscv sp must be 16-byte aligned
    if (sp < stackbase) goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase) goto bad;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/') last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp;          // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  if (p->pid == 1) vmprint(p->pagetable);

  return argc;  // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (pagetable) proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

void vmprint(pagetable_t pgtbl) {
  // 打印页表的物理地址
  printf("page table %p\n", pgtbl);
  // 从根页表开始遍历，level=2表示根级，va_base=0表示虚拟地址从0开始，depth=0表示层级深度为0
  vmprint_walk(pgtbl, 2, 0, 0);
}

// 递归遍历页表并打印页表项的函数
// level: 当前页表层级 (2=根级, 1=中级, 0=叶子级)
// va_base: 当前层级的虚拟地址基址
// depth: 递归深度，用于格式化输出 (0=根级, 1=第一级子页表, 等等)
static void vmprint_walk(pagetable_t pagetable, int level, uint64 va_base, int depth) {
  // 遍历当前页表的512个页表项
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if (!(pte & PTE_V)) continue; // 跳过无效的页表项
    
    // 构建前缀字符串，显示正确数量的 ||
    printf("||");
    for (int d = 0; d < depth; d++) {
      printf("   ||");
    }
    
    // 提取物理地址并构建权限标志字符串
    uint64 pa = PTE2PA(pte);
    char flags[5];
    flags[0] = (pte & PTE_R) ? 'r' : '-';  // 读权限
    flags[1] = (pte & PTE_W) ? 'w' : '-';  // 写权限
    flags[2] = (pte & PTE_X) ? 'x' : '-';  // 执行权限
    flags[3] = (pte & PTE_U) ? 'u' : '-';  // 用户态权限
    flags[4] = '\0';
    
    // 检查这是否是叶子节点（设置了R、W或X位）
    if (pte & (PTE_R | PTE_W | PTE_X)) {
      // 这是叶子节点 - 打印虚拟地址到物理地址的映射
      uint64 va = va_base + ((uint64)i << (12 + level * 9));
      printf("idx: %d: va: %p -> pa: %p, flags: %s\n", i, va, pa, flags);
    } else {
      // 这是非叶子节点 - 只打印物理地址
      printf("idx: %d: pa: %p, flags: %s\n", i, pa, flags);
      
      // 如果不是叶子级别，递归到下一级
      if (level > 0) {
        uint64 child_va_base = va_base + ((uint64)i << (12 + level * 9));
        vmprint_walk((pagetable_t)pa, level - 1, child_va_base, depth + 1);
      }
    }
  }
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
  uint i, n;
  uint64 pa;

  if ((va % PGSIZE) != 0) panic("loadseg: va must be page aligned");

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0) panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n) return -1;
  }

  return 0;
}
