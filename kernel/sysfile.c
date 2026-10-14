#include "types.h"
#include "riscv.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

/*
 * ============================================================================
 * sysfile.c - 文件与输入输出相关系统调用实现
 * ============================================================================
 * 本文件负责处理与文件系统、控制台 I/O 以及程序加载执行相关的系统调用。
 * 在操作系统教学实验的当前阶段：
 * 1. 完整的磁盘文件系统尚未引入（计划于 Lab 6 实现）。
 * 2. 所有的 I/O 均重定向至控制台（fd 0 为标准输入，fd 1/2 为标准输出/错误）。
 * 3. 进程执行 sys_exec() 采用嵌入式扁平二进制（Flat Binary）机制，直接从
 *    内核镜像中查找并加载用户程序，无需经过磁盘文件系统解析 ELF 头部。
 * ============================================================================
 */

// 外部函数声明: 在内核内嵌的用户程序表中查找指定名称的程序二进制
// 入参: name - 程序名称 (如 "sh", "echo")
// 出参: pstart - 程序二进制在内核内存中的起始物理/虚拟地址
//       pend   - 程序二进制在内核内存中的结束地址
// 返回: 0 表示找到, -1 表示未找到
extern int uprog_find(const char *name, uint64 *pstart, uint64 *pend);

/*
 * sys_exec - 执行新程序系统调用 (exec)
 *
 * 功能描述:
 *   用指定的可执行程序替换当前进程的内存映像（代码、数据、堆栈等），
 *   并从新程序的起始位置（虚拟地址 0）开始执行。该调用成功时不返回，
 *   只有失败时才会返回 -1。
 *
 * 在本实验架构中的简化实现流程:
 *   1. 获取用户态传入的程序路径参数 path 和参数数组指针 uargv。
 *   2. 清理 path 末尾的换行符/空白符（针对 shell gets 的输入特性）。
 *   3. 调用 uprog_find() 在内核嵌入的二进制表中找到对应程序。
 *   4. 计算所需虚拟内存大小 = 代码与数据大小 (按页向上对齐) + 1 页用户栈。
 *   5. 为当前进程创建全新的页表，并分配物理内存映射到虚拟地址 [0, newsz)。
 *   6. 使用 copyout() 将内核中的二进制内容拷贝到新页表的用户虚拟地址 0 处。
 *   7. 切换进程页表为新页表，重置 trapframe 中的指令计数器 (epc=0) 和栈指针 (sp=newsz)。
 *   8. 释放旧页表及其物理内存，并更新进程名称。
 */
uint64
sys_exec(void)
{
  char path[128];
  uint64 uargv;

  // 1. 获取系统调用的第 0 个参数 (目标程序路径字符串)
  if (argstr(0, path, sizeof(path)) < 0)
    return -1;
  // 获取第 1 个参数 (参数列表指针 argv, 当前实验简化暂不解析参数传递)
  argaddr(1, &uargv);

  // 2. 去除尾部换行符、回车符和空格
  // 原因: 用户在 shell 终端通过 gets 输入命令时，末尾通常带有 '\n' 或多余空格，
  // 需要裁剪干净才能与程序表中的程序名 (如 "sh") 精确匹配。
  int len = strlen(path);
  while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r' || path[len - 1] == ' ')) {
    path[len - 1] = '\0';
    len--;
  }

  // 3. 在内核内嵌的二进制镜像中检索用户程序
  uint64 start = 0, end = 0;
  if (uprog_find(path, &start, &end) < 0) {
    // 找不到该程序，执行失败
    return -1;
  }

  // 4. 计算新程序所需的内存大小
  // sz: 程序本身的代码和数据段大小 (字节数)
  uint64 sz = end - start;
  // newsz: 程序占用的页面向上对齐后，额外再加上 1 个页面作为用户态运行栈 (User Stack)
  uint64 newsz = PGROUNDUP(sz) + PGSIZE; // 程序代码/数据 + 用户栈页

  struct proc *p = myproc();

  // 5. 为新程序创建全新的空页表 (初始化映射 trampoline 和 trapframe 等关键内核结构)
  pagetable_t pagetable = proc_pagetable(p);
  if (pagetable == 0)
    return -1;

  // 6. 分配物理内存并建立虚拟地址映射:
  // 范围: 虚拟地址 0 到 newsz
  // 权限: 可写 (PTE_W) | 可执行 (PTE_X) | 用户权限 (PTE_U) | 可读 (PTE_R)
  if (uvmalloc(pagetable, 0, newsz, PTE_W | PTE_X) == 0) {
    proc_freepagetable(pagetable, 0);
    return -1;
  }

  // 7. 将内核中保存的扁平二进制代码拷贝到用户新页表的虚拟地址 0 处
  copyout(pagetable, 0, (char *)start, sz);

  // 8. 切换进程的页表和大小:
  // 先备份旧页表指针与大小，然后将进程指向新页表
  pagetable_t oldpagetable = p->pagetable;
  uint64 oldsz = p->sz;
  p->pagetable = pagetable;
  p->sz = newsz;

  // 9. 重置中断返回现场 (Trapframe):
  // epc (Exception Program Counter): 用户态程序入口地址设置为 0 (二进制从 0 开始执行)
  // sp (Stack Pointer): 用户栈指针设置在最高地址 newsz 处 (RISC-V 栈从高地址向低地址增长)
  p->trapframe->epc = 0;
  p->trapframe->sp = newsz;

  // 10. 释放原先旧进程占用的旧页表及其所有物理内存页面
  proc_freepagetable(oldpagetable, oldsz);

  // 11. 记录进程名称 (便于调试与 ps 打印)
  safestrcpy(p->name, path, sizeof(p->name));
  return 0; // 成功返回 0 (返回给 trapframe 恢复后的用户态)
}

/*
 * sys_read - 读文件/输入流系统调用 (read)
 *
 * 语法: int read(int fd, void *buf, int n);
 * 入参:
 *   fd - 文件描述符 (0 为标准输入 stdin)
 *   p  - 用户空间缓冲区虚拟地址 (存放读入数据的地址)
 *   n  - 请求读取的最大字节数
 * 返回值:
 *   成功读取的字节数，或者 -1 表示错误
 */
uint64
sys_read(void)
{
  int fd, n;
  uint64 p;

  // 解析系统调用传入的 3 个参数
  argint(0, &fd);
  argaddr(1, &p);
  argint(2, &n);

  if (fd == 0) {
    // fd == 0 表示标准输入 stdin
    // 委托给控制台驱动的 consoleread() 读取键盘/串口输入
    // 参数 1 表示目标地址 p 位于用户虚拟地址空间
    return consoleread(1, p, n);
  }

  // 尚未支持普通磁盘文件读操作，非 0 文件描述符返回失败
  return -1;
}

/*
 * sys_write - 写文件/输出流系统调用 (write)
 *
 * 语法: int write(int fd, const void *buf, int n);
 * 入参:
 *   fd - 文件描述符 (1 为标准输出 stdout, 2 为标准错误 stderr)
 *   p  - 用户空间缓冲区虚拟地址 (包含待输出数据的地址)
 *   n  - 请求写入的字节数
 * 返回值:
 *   成功写入的字节数，或者 -1 表示错误
 */
uint64
sys_write(void)
{
  int fd, n;
  uint64 p;

  // 解析系统调用传入的 3 个参数
  argint(0, &fd);
  argaddr(1, &p);
  argint(2, &n);

  if (fd == 1 || fd == 2) {
    // fd == 1 (stdout) 或 fd == 2 (stderr)
    // 委托给控制台驱动的 consolewrite() 将字符发送至串口 UART
    // 参数 1 表示源数据地址 p 位于用户虚拟地址空间
    return consolewrite(1, p, n);
  }

  // 尚未支持普通磁盘文件写操作，其他文件描述符返回失败
  return -1;
}

/*
 * ============================================================================
 * 尚未实现的底层文件系统调用桩函数 (Lab 6 实验目标)
 * 当前阶段安全返回 -1 (表示调用失败/未实现)，防止未定义行为。
 * ============================================================================
 */

// 创建管道 (pipe): 在进程间建立基于单向数据流的 IPC 通道
uint64 sys_pipe(void)   { return -1; }

// 获取文件状态 (fstat): 读取文件的元信息 (如文件类型、大小、inode 编号等)
uint64 sys_fstat(void)  { return -1; }

// 改变当前工作目录 (chdir): 修改进程上下文中记录的当前目录 inode (cwd)
uint64 sys_chdir(void)  { return -1; }

// 复制文件描述符 (dup): 分配一个新的文件描述符并指向相同的已打开文件结构
uint64 sys_dup(void)    { return -1; }

// 打开或创建文件 (open): 根据路径查找或创建 inode，在进程文件表中分配描述符
uint64 sys_open(void)   { return -1; }

// 创建设备文件 (mknod): 在文件系统中创建关联特定主/次设备号的特殊文件
uint64 sys_mknod(void)  { return -1; }

// 删除文件硬链接/删除文件 (unlink): 移除目录项，若链接数为 0 则释放文件数据
uint64 sys_unlink(void) { return -1; }

// 创建硬链接 (link): 为现有文件的 inode 在指定路径下创建新的目录项
uint64 sys_link(void)   { return -1; }

// 创建目录 (mkdir): 在文件系统中新建目录并初始化 "." 和 ".." 目录项
uint64 sys_mkdir(void)  { return -1; }

// 关闭文件描述符 (close): 释放进程文件表中对应的文件引用
uint64 sys_close(void)  { return -1; }

// 同步磁盘缓存 (sync): 将内存中脏的块缓冲区 (buffer cache) 强制刷写到持久化磁盘
uint64 sys_sync(void)   { return -1; }

