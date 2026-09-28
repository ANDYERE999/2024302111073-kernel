#include "types.h"
#include "riscv.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern int uprog_find(const char *name, uint64 *pstart, uint64 *pend);

uint64
sys_exec(void)
{
  char path[128];
  uint64 uargv;

  if (argstr(0, path, sizeof(path)) < 0)
    return -1;
  argaddr(1, &uargv);

  // 去除尾部换行符、回车符和空格 (处理 shell 输入 gets 附带的 '\n')
  int len = strlen(path);
  while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r' || path[len - 1] == ' ')) {
    path[len - 1] = '\0';
    len--;
  }

  uint64 start = 0, end = 0;
  if (uprog_find(path, &start, &end) < 0) {
    return -1;
  }

  uint64 sz = end - start;
  uint64 newsz = PGROUNDUP(sz) + PGSIZE; // 程序代码/数据 + 用户栈页

  struct proc *p = myproc();
  pagetable_t pagetable = proc_pagetable(p);
  if (pagetable == 0)
    return -1;

  if (uvmalloc(pagetable, 0, newsz, PTE_W | PTE_X) == 0) {
    proc_freepagetable(pagetable, 0);
    return -1;
  }

  // 将内嵌平铺二进制加载至用户虚拟地址 0
  copyout(pagetable, 0, (char *)start, sz);

  // 释放原进程页表与内存，切换至新页表
  pagetable_t oldpagetable = p->pagetable;
  uint64 oldsz = p->sz;
  p->pagetable = pagetable;
  p->sz = newsz;

  // 伪造第一现场: 程序入口为 0, 用户栈在顶部按 16 字节对齐
  p->trapframe->epc = 0;
  p->trapframe->sp = newsz;

  proc_freepagetable(oldpagetable, oldsz);

  safestrcpy(p->name, path, sizeof(p->name));
  return 0;
}

uint64
sys_read(void)
{
  int fd, n;
  uint64 p;

  argint(0, &fd);
  argaddr(1, &p);
  argint(2, &n);

  if (fd == 0) {
    // 读标准输入 (控制台环形缓冲区)
    return consoleread(1, p, n);
  }
  return -1;
}

uint64
sys_write(void)
{
  int fd, n;
  uint64 p;

  argint(0, &fd);
  argaddr(1, &p);
  argint(2, &n);

  if (fd == 1 || fd == 2) {
    // 写标准输出 / 标准错误 (控制台/UART 打印)
    return consolewrite(1, p, n);
  }
  return -1;
}

// 尚未实现的底层文件系统调用 (Lab 6 目标)，安全返回 -1
uint64 sys_pipe(void)   { return -1; }
uint64 sys_fstat(void)  { return -1; }
uint64 sys_chdir(void)  { return -1; }
uint64 sys_dup(void)    { return -1; }
uint64 sys_open(void)   { return -1; }
uint64 sys_mknod(void)  { return -1; }
uint64 sys_unlink(void) { return -1; }
uint64 sys_link(void)   { return -1; }
uint64 sys_mkdir(void)  { return -1; }
uint64 sys_close(void)  { return -1; }
uint64 sys_sync(void)   { return -1; }
