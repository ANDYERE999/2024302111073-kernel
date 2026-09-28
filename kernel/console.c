#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "course_sid.h"

/*
 * UART 16550A 寄存器映射定义 (基地址 0x10000000)
 */
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))
#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

/* 寄存器偏移量定义 */
#define RHR 0 // Receive Holding Register (读)
#define THR 0 // Transmit Holding Register (写)
#define IER 1 // Interrupt Enable Register
#define FCR 2 // FIFO Control Register
#define LCR 3 // Line Control Register
#define LSR 5 // Line Status Register

/* 关键标志位 */
#define LCR_DLAB       (1 << 7) // 除数锁存器访问位(设置波特率)
#define LCR_EIGHT_BITS (3 << 0) // 8位数据位, 无校验, 1位停止位
#define FCR_ENABLE_FIFO (1 << 0) // 启用 FIFO
#define FCR_CLEAR_FIFO  (3 << 1) // 清除 RX/TX FIFO
#define LSR_THRE       (1 << 5) // bit 5: Transmit Holding Register Empty (发送就绪)
#define LSR_RX_READY   (1 << 0) // bit 0: Receive data ready

/*
 * 节流周期宏定义 (严禁硬编码数字, 必须引用 COURSE_SID 宏)
 * 周期 = 16 + (COURSE_SID % 16)
 */
#define THROTTLE_INTERVAL (16 + ((COURSE_SID) % 16))

/* 全局累计发送字节计数器, 用于节流控制 */
static int tx_byte_count = 0;

/*
 * 控制台环形输入缓冲区结构定义
 * 大小由 LAB2_BUF_SIZE 宏指定 (个性化值 64 字节)
 */
struct {
  struct spinlock lock;
  char buf[LAB2_BUF_SIZE];
  uint r; // 读者已消费下标
  uint w; // 已提交可读下标
  uint e; // 当前编辑/输入下标
} cons;

/*
 * uartinit: 16550A UART 硬件底层初始化
 */
void
uartinit(void)
{
  // 1. 关闭中断以安全配置波特率
  WriteReg(IER, 0x00);

  // 2. 进入设置波特率模式 (DLAB = 1)
  WriteReg(LCR, LCR_DLAB);

  // 3. 配置波特率为 38400 (时钟分频系数为 3)
  WriteReg(0, 0x03); // LSB
  WriteReg(1, 0x00); // MSB

  // 4. 退出设置波特率模式, 设置数据格式为 8N1 (8位数据, 无校验, 1停止位)
  WriteReg(LCR, LCR_EIGHT_BITS);

  // 5. 复位并启用 FIFO 队列
  WriteReg(FCR, FCR_ENABLE_FIFO | FCR_CLEAR_FIFO);

  // 6. 开启串口接收就绪中断 (IER bit 0)
  WriteReg(IER, 0x01);
}

/*
 * uartputc_sync: 单字节同步硬件输出
 * 协议规范:
 * - 轮询等待 LSR bit 5 (THRE) 为 1 时写入 THR 寄存器
 * - 满足节流周期时执行指定数量的 nop 循环
 */
void
uartputc_sync(char c)
{
  // 忙等轮询直到发送保持寄存器空闲
  while ((ReadReg(LSR) & LSR_THRE) == 0)
    ;

  // 写入字符至发送保持寄存器
  WriteReg(THR, c);

  // 空转节流检查与注入
  tx_byte_count++;
  if (tx_byte_count >= THROTTLE_INTERVAL) {
    tx_byte_count = 0;
    // 执行指定 nop 空转循环
    for (volatile int i = 0; i < 100; i++) {
      asm volatile("nop");
    }
  }
}

/*
 * consputc: 控制台层输出接口
 */
void
consputc(int c)
{
  uartputc_sync((char)c);
}

/*
 * consoleinit: 控制台模块初始化
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
 * uartintr: 串口硬件接收中断处理函数
 * 当 UART 接收到字符触发 PLIC 中断时被调用
 */
void
uartintr(void)
{
  while (ReadReg(LSR) & LSR_RX_READY) {
    int c = ReadReg(RHR);
    consoleintr(c);
  }
}

/*
 * consoleintr: 将串口输入的字符压入环形缓冲区并按语义唤醒读者
 * 支持行缓冲 (LAB2_BUF_SEMANTICS == 0) 与字符流 (LAB2_BUF_SEMANTICS == 1)
 */
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch (c) {
  case 0x7f: // Backspace (DEL)
  case '\b': // Backspace
    if (cons.e > cons.w) {
      cons.e--;
      consputc('\b');
      consputc(' ');
      consputc('\b');
    }
    break;

  default:
    if (c != 0 && (cons.e - cons.r) < LAB2_BUF_SIZE) {
      if (c == '\r')
        c = '\n';

      // 屏幕字符回显
      consputc(c);

      // 存入环形缓冲区
      cons.buf[cons.e++ % LAB2_BUF_SIZE] = c;

#if LAB2_BUF_SEMANTICS == 0
      // 协议 0 (行缓冲): 仅当按下回车 '\n' 或 EOF (Ctrl-D) 时才提交并唤醒读者
      if (c == '\n' || c == 0x04) {
        cons.w = cons.e;
        wakeup(&cons.r);
      }
#else
      // 协议 1 (字符流): 每按一个键即刻提交并唤醒读者
      cons.w = cons.e;
      wakeup(&cons.r);
#endif
    }
    // 缓冲区满时的丢弃处理: 丢弃字符, 不 panic (文档化策略)
    break;
  }

  release(&cons.lock);
}

/*
 * consoleread: 用户态系统调用 read(0, buf, n) 的内核实现
 * 从控制台环形缓冲区取数据
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
    // 读者等待数据到来
    while (cons.r == cons.w) {
      if (killed(myproc())) {
        release(&cons.lock);
        return -1;
      }
#if LAB2_BUF_SEMANTICS == 1
      // 字符流语义: 若此前已读到数据, 且当前已无更多积压字符, 则立即返回读者
      if (target > (uint)n) {
        release(&cons.lock);
        return target - n;
      }
#endif
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % LAB2_BUF_SIZE];

    // 处理 EOF (Ctrl-D)
    if (c == 0x04) {
      if (n < (int)target) {
        // 保存 EOF 留待下一次 read 触发
        cons.r--;
      }
      break;
    }

    // 将字符复制到用户态缓冲区
    cbuf = c;
    if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if (c == '\n') {
      // 读完一行, 提前返回
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

/*
 * consolewrite: 用户态系统调用 write(1/2, buf, n) 的内核实现
 * 将用户态缓冲区数据写到终端
 */
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  for (i = 0; i < n; i++) {
    char c;
    if (either_copyin(&c, user_src, src + i, 1) == -1)
      break;
    uartputc_sync(c);
  }

  return i;
}
