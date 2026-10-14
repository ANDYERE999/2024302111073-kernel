#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "course_sid.h"

/*
 * ============================================================================
 * console.c - 控制台与 UART 16550A 驱动实现
 * ============================================================================
 * 本文件负责实现操作系统底层的终端控制台输入/输出子系统，整体架构分为三层：
 *
 * 1. 硬件通信层 (UART 16550A 串口芯片):
 *    - 通过内存映射 I/O (MMIO, Memory-Mapped I/O) 直接读写串口硬件寄存器。
 *    - 基地址定义在 memlayout.h 中 (UART0 = 0x10000000)。
 *    - 发送采用轮询忙等 (uartputc_sync)，接收采用中断驱动 (uartintr)。
 *
 * 2. 环形缓冲区与行编辑层 (Console Ring Buffer):
 *    - 维护一个容量为 LAB2_BUF_SIZE (64 字节) 的环形缓冲区 cons。
 *    - 采用三指针设计 (r, w, e):
 *        * r (Read): 读者已消费下标 (已被用户程序 read 读取)
 *        * w (Write): 已提交可读下标 (已确认，允许用户程序读取)
 *        * e (Edit): 当前编辑下标 (正在输入的键盘字符，尚未全部提交)
 *    - 指针关系始终保持: r <= w <= e (在模运算展开的逻辑序号上)。
 *    - 支持退格键 (Backspace) 擦除未提交字符 [w, e)。
 *    - 支持两种缓冲语义:
 *        * LAB2_BUF_SEMANTICS == 0: 行缓冲 (按下回车或 EOF 时才提交唤醒读者)
 *        * LAB2_BUF_SEMANTICS == 1: 字符流 (按下一个键即刻提交唤醒读者)
 *
 * 3. 系统调用接口层 (Syscall Interface):
 *    - consoleread(): 对应 read(0, buf, n)，从控制台环形缓冲区阻塞读取。
 *    - consolewrite(): 对应 write(1/2, buf, n)，将用户数据输出至串口。
 * ============================================================================
 */

/*
 * UART 16550A 寄存器映射宏定义 (基地址 0x10000000)
 *
 * 注意:
 * 1. 使用 volatile 关键字防止编译器将硬件 I/O 读写优化或缓存到 CPU 寄存器中。
 * 2. 在 QEMU virt 平台中，UART 寄存器每隔 1 个字节排列 (偏移 0, 1, 2, ...)。
 */
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))
#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

/*
 * 16550A 寄存器偏移量定义 (Offsets)
 * 某些偏移量在不同状态 (DLAB 位) 下具有不同含义:
 */
#define RHR 0 // Receive Holding Register (只读, DLAB=0): 接收保持寄存器，读取串口收到的字节
#define THR 0 // Transmit Holding Register (只写, DLAB=0): 发送保持寄存器，写入待发送的字节
#define IER 1 // Interrupt Enable Register (读写, DLAB=0): 中断使能寄存器
#define FCR 2 // FIFO Control Register (只写): FIFO 控制寄存器 (启用/清除硬件缓存队列)
#define LCR 3 // Line Control Register (读写): 线路控制寄存器 (设置数据位宽、校验位及 DLAB)
#define LSR 5 // Line Status Register (只读): 线路状态寄存器 (指示发送/接收状态标志)

/*
 * 关键寄存器标志位 (Bit Flags)
 */
#define LCR_DLAB       (1 << 7) // bit 7: Divisor Latch Access Bit (除数锁存器访问位, 置 1 后偏移 0,1 表示波特率分频系数)
#define LCR_EIGHT_BITS (3 << 0) // bit 1:0 = 11b: 8 位数据位, 无奇偶校验, 1 位停止位 (8N1 格式)
#define FCR_ENABLE_FIFO (1 << 0) // bit 0: 启用发送和接收的 16 字节硬件 FIFO 队列
#define FCR_CLEAR_FIFO  (3 << 1) // bit 2:1 = 11b: 同时复位/清空接收与发送 FIFO
#define LSR_THRE       (1 << 5) // bit 5: Transmit Holding Register Empty (发送就绪标志, 1 表示 THR 为空可写入)
#define LSR_RX_READY   (1 << 0) // bit 0: Data Ready (接收就绪标志, 1 表示已收到数据，RHR 可读)

/*
 * 节流周期宏定义 (实验特定规则)
 * 公式: 周期 = 16 + (COURSE_SID % 16)
 * 作用: 控制串口输出吞吐节奏，满足自动化测试用例的时序要求。
 */
#define THROTTLE_INTERVAL (16 + ((COURSE_SID) % 16))

/* 全局累计发送字节计数器, 用于节流控制 (每达到 THROTTLE_INTERVAL 字节执行一次空转) */
static int tx_byte_count = 0;

/*
 * 控制台环形输入缓冲区结构定义
 *
 * 环形缓冲区的核心状态机 (3-Pointer Ring Buffer):
 *
 *              已消费区域          待读取区域(已提交)       编辑中区域(可退格)         空闲区域
 *        [..................|====================|####################|..................]
 *                           ^                    ^                    ^
 *                         cons.r               cons.w               cons.e
 *
 * - r: 读者读取位置 (consoleread 从此位置取数据)
 * - w: 已提交写入位置 (用户已经输入并确定的字符，如按下了回车或 EOF)
 * - e: 当前编辑位置 (键盘刚按下的字符，若用户按退格键，e 可以回退至 w)
 * - lock: 自旋锁，保证多核 CPU 以及硬件中断并发访问缓冲区时的线程安全性
 */
struct {
  struct spinlock lock;
  char buf[LAB2_BUF_SIZE]; // 环形存储数组，大小由 LAB2_BUF_SIZE 宏指定 (64 字节)
  uint r; // 读者已消费下标 (Read index)
  uint w; // 已提交可读下标 (Write/Committed index)
  uint e; // 当前编辑/输入下标 (Edit index)
} cons;

/*
 * uartinit: 16550A UART 硬件底层初始化
 *
 * 硬件初始化标准流程:
 * 1. 关闭中断，防止配置过程中产生意外中断。
 * 2. 开启 DLAB 模式，向偏移 0 和 1 写入波特率分频系数 (Divisor)。
 *    分频系数计算公式: Divisor = 时钟频率 / (16 * 波特率)
 *    QEMU 中 UART 时钟为 1.8432 MHz，分频系数 3 对应波特率 38400。
 * 3. 关闭 DLAB 模式，并配置字符格式为 8N1 (8位数据，无校验，1位停止位)。
 * 4. 启用并重置硬件 FIFO 队列 (16 字节缓存)。
 * 5. 开启串口接收就绪中断 (IER bit 0)，以便键盘输入时触发 CPU 外部中断。
 */
void
uartinit(void)
{
  // 1. 关闭所有中断以安全配置波特率
  WriteReg(IER, 0x00);

  // 2. 进入设置波特率模式 (将 LCR 寄存器的 DLAB 位置 1)
  WriteReg(LCR, LCR_DLAB);

  // 3. 配置波特率为 38400 (时钟分频系数为 3, 16位整数拆分为高低8位写入)
  WriteReg(0, 0x03); // LSB (低字节): 3
  WriteReg(1, 0x00); // MSB (高字节): 0

  // 4. 退出设置波特率模式 (清空 DLAB), 设置数据格式为 8N1 (8位数据, 无校验, 1位停止位)
  WriteReg(LCR, LCR_EIGHT_BITS);

  // 5. 复位并启用接收与发送 FIFO 队列
  WriteReg(FCR, FCR_ENABLE_FIFO | FCR_CLEAR_FIFO);

  // 6. 开启串口接收数据就绪中断 (IER bit 0: Received Data Available Interrupt)
  WriteReg(IER, 0x01);
}

/*
 * uartputc_sync: 单字节同步硬件输出
 *
 * 实现机制:
 * 1. 采用轮询 (Polling) / 忙等机制: 循环读取 LSR 寄存器，直到 bit 5 (THRE) 为 1，
 *    确认 UART 发送保持寄存器已空闲，然后写入待发送字符 c。
 * 2. 节流控制: 每连续输出 THROTTLE_INTERVAL 字节后，重置计数器并插入 100 次 nop 指令，
 *    产生微小空转延时以满足评测系统要求。
 */
void
uartputc_sync(char c)
{
  // 忙等轮询直到发送保持寄存器空闲 (THRE 为 1)
  while ((ReadReg(LSR) & LSR_THRE) == 0)
    ;

  // 写入字符至发送保持寄存器 (硬件开始通过串行线路传输该字节)
  WriteReg(THR, c);

  // 空转节流检查与注入延时
  tx_byte_count++;
  if (tx_byte_count >= THROTTLE_INTERVAL) {
    tx_byte_count = 0;
    // 执行指定数量的 nop 指令产生短暂停顿
    for (volatile int i = 0; i < 100; i++) {
      asm volatile("nop");
    }
  }
}

/*
 * consputc: 控制台层字符输出接口
 * 封装底层的硬件串口同步输出函数 uartputc_sync
 */
void
consputc(int c)
{
  uartputc_sync((char)c);
}

/*
 * consoleinit: 控制台模块初始化
 *
 * 流程:
 * 1. 初始化控制台自旋锁 cons.lock。
 * 2. 将环形缓冲区的 r, w, e 下标重置为 0。
 * 3. 调用 uartinit() 完成 16550A 硬件初始化。
 */
void
consoleinit(void)
{
  initlock(&cons.lock, "cons");
  cons.r = 0;
  cons.w = 0;
  cons.e = 0;
  uartinit();
}

/*
 * uartintr: 串口硬件接收中断处理函数 (Top-half / 中断服务例程)
 *
 * 触发条件: 当用户在终端键入字符，UART 芯片接收到字节并通过 PLIC 触发外部中断。
 * 处理逻辑: 循环检查 LSR 的 RX_READY 位，不断将 RHR 寄存器中的所有可用字节取出，
 *           交由上层的 consoleintr() 处理。
 */
void
uartintr(void)
{
  while (ReadReg(LSR) & LSR_RX_READY) {
    int c = ReadReg(RHR); // 从接收保持寄存器中读取一个硬件字符
    consoleintr(c);        // 传入控制台输入状态机
  }
}

/*
 * consoleintr: 控制台字符输入处理状态机 (Lab 2 核心部分)
 *
 * 参数: c - 串口接收到的原始 ASCII 字符编码
 *
 * 处理逻辑:
 * 1. 获取 cons.lock 自旋锁，保护环形缓冲区状态。
 * 2. 特殊字符处理:
 *    - 退格键 (0x7f DEL 或 '\b'):
 *      若当前有未提交的编辑字符 (cons.e > cons.w)，则编辑下标递减 cons.e--，
 *      并在终端屏幕上回显 "\b \b" (退格-空格-退格) 擦除前一个字符的视觉显示。
 * 3. 普通字符处理:
 *    - 检查缓冲区是否已满: 当 (cons.e - cons.r) < LAB2_BUF_SIZE 时才允许接收。
 *      若缓冲区已满，则静默丢弃字符，防止缓冲区溢出或内核 panic。
 *    - 回车符转换: 终端输入的 '\r' 统一转换为标准换行符 '\n'。
 *    - 屏幕字符回显: 调用 consputc(c) 让用户在终端看到自己键入的字符。
 *    - 存入环形缓冲区: cons.buf[cons.e++ % LAB2_BUF_SIZE] = c。
 *    - 缓冲语义处理:
 *      * 模式 0 (行缓冲): 仅当遇到换行符 '\n' 或 EOF (Ctrl-D, 0x04) 时，
 *        才将当前输入提交 cons.w = cons.e 并唤醒等待读取的读者进程 wakeup(&cons.r)。
 *      * 模式 1 (字符流): 每输入一个有效字符，立即提交 cons.w = cons.e 并唤醒读者。
 * 4. 释放 cons.lock 自旋锁。
 */
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch (c) {
  case 0x7f: // Backspace (ASCII 127, DEL 键)
  case '\b': // Backspace (ASCII 8, 退格键)
    // 只有在当前编辑行内还有未提交字符时，才允许回退删除
    if (cons.e > cons.w) {
      cons.e--;
      // 终端退格擦除视觉三部曲: 光标左移 -> 打印空格覆盖原字符 -> 光标再次左移
      consputc('\b');
      consputc(' ');
      consputc('\b');
    }
    break;

  default:
    // 字符非空且环形缓冲区尚有剩余空间
    if (c != 0 && (cons.e - cons.r) < LAB2_BUF_SIZE) {
      // 终端输入的换行通常是 '\r' (CR), 统一标准化为 UNIX 换行符 '\n' (LF)
      if (c == '\r')
        c = '\n';

      // 屏幕字符回显 (让用户终端直观看到敲击的字符)
      consputc(c);

      // 存入环形缓冲区当前编辑下标位置，并推进编辑指针 e
      cons.buf[cons.e++ % LAB2_BUF_SIZE] = c;

#if LAB2_BUF_SEMANTICS == 0
      // 协议 0 (规范模式/行缓冲): 仅当按下回车 '\n' 或 EOF (Ctrl-D, 0x04) 时才提交并唤醒读者
      if (c == '\n' || c == 0x04) {
        cons.w = cons.e;
        wakeup(&cons.r);
      }
#else
      // 协议 1 (非规范模式/字符流): 每按一个键即刻提交到可读区域并唤醒读者
      cons.w = cons.e;
      wakeup(&cons.r);
#endif
    }
    // 缓冲区满时的丢弃处理策略: 丢弃溢出字符, 不 panic, 保证内核稳定性
    break;
  }

  release(&cons.lock);
}

/*
 * consoleread: 用户态系统调用 read(0, buf, n) 的内核实现 (Lab 2 核心部分)
 *
 * 功能描述:
 *   从控制台输入缓冲区中阻塞读取最多 n 个字节的数据，拷贝至目标地址 dst 中。
 *
 * 参数:
 *   user_dst - 目标地址是否位于用户空间 (1 为用户态虚拟地址, 0 为内核态虚拟地址)
 *   dst      - 接收数据的目标缓冲区地址
 *   n        - 请求读取的最大字节数
 *
 * 返回值:
 *   成功读取并拷贝的字节数；若进程被杀或出错则返回 -1。
 *
 * 核心逻辑:
 * 1. 读者进入阻塞等待: 只要没有可读数据 (cons.r == cons.w)，就调用 sleep(&cons.r, &cons.lock)
 *    让出 CPU，直到 consoleintr() 中产生新数据并调用 wakeup(&cons.r)。
 * 2. 检查进程是否已被杀死 (killed(myproc()))，若是则立刻释放锁退出并返回 -1。
 * 3. 字符流语义特殊处理: 若处于字符流模式下且此前已经读到了部分数据，但当前缓冲区暂无积压，
 *    则无需继续阻塞等待，直接提前返回已读数据。
 * 4. 从环形缓冲区取出字符 c = cons.buf[cons.r++ % LAB2_BUF_SIZE]。
 * 5. EOF (Ctrl-D, 0x04) 处理:
 *    - 若此前已有字符被读取 (n < target)，则将 EOF 放回 (cons.r--)，优先返回当前行字符；
 *    - 下一次调用 read() 时由于起始字符就是 EOF，将直接 break 并返回 0 字节 (标准 UNIX EOF 语义)。
 * 6. 使用 either_copyout() 将字符安全拷贝至用户地址空间 dst。
 * 7. 遇到换行符 '\n' 时提前结束本次读取并返回，保证行读取接口 (如 gets) 正常工作。
 */
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while (n > 0) {
    // 当读者下标追上已提交下标时，表示当前无新数据可读，需阻塞等待
    while (cons.r == cons.w) {
      // 检查当前进程是否已被标记杀死 (例如收到终止信号)
      if (killed(myproc())) {
        release(&cons.lock);
        return -1;
      }
#if LAB2_BUF_SEMANTICS == 1
      // 字符流语义: 若此前已经读到数据 (target > n)，且当前已无更多积压字符，则立即返回已读字节
      if (target > (uint)n) {
        release(&cons.lock);
        return target - n;
      }
#endif
      // 释放 cons.lock 并在 cons.r 条件变量上进入睡眠，等待中断唤醒
      sleep(&cons.r, &cons.lock);
    }

    // 从环形缓冲区取出下一个可读字符，并推进读者指针 r
    c = cons.buf[cons.r++ % LAB2_BUF_SIZE]; // 缓冲区容量 LAB2_BUF_SIZE (64 字节)

    // 处理 EOF 控制字符 (Ctrl-D, ASCII 0x04)
    if (c == 0x04) {
      if (n < (int)target) {
        // 如果本次 read 已经读取了其他字符，则保留 EOF 留待下一次 read 触发
        cons.r--;
      }
      // 退出循环 (若本次 read 第一个字符就是 EOF，则循环退出且读取字节数为 0，符合 EOF 规范)
      break;
    }

    // 将字符安全复制到目标缓冲区 (用户态或内核态)
    cbuf = c;
    if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    // 读完一行 (遇到换行符)，提前返回给上层用户程序
    if (c == '\n') {
      break;
    }
  }
  release(&cons.lock);

  // 返回实际读取成功的字节数
  return target - n;
}

/*
 * consolewrite: 用户态系统调用 write(1/2, buf, n) 的内核实现
 *
 * 功能描述:
 *   从源缓冲区 src 拷贝 n 个字节的数据，并通过串口硬件逐字节输出至控制台屏幕。
 *
 * 参数:
 *   user_src - 源数据是否位于用户空间 (1 为用户态虚拟地址, 0 为内核态虚拟地址)
 *   src      - 存放待输出数据的源缓冲区虚拟地址
 *   n        - 请求写入的字节数
 *
 * 返回值:
 *   实际写入的字节数
 */
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  for (i = 0; i < n; i++) {
    char c;
    // 从源缓冲区安全拷贝 1 字节到内核栈变量 c
    if (either_copyin(&c, user_src, src + i, 1) == -1)
      break;
    // 通过串口硬件同步输出字符
    uartputc_sync(c);
  }

  return i;
}

