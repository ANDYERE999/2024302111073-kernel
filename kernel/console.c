#include "types.h"
#include "memlayout.h"
#include "riscv.h"
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

/*
 * 节流周期宏定义 (严禁硬编码数字, 必须引用 COURSE_SID 宏)
 * 周期 = 16 + (COURSE_SID % 16)
 */
#define THROTTLE_INTERVAL (16 + ((COURSE_SID) % 16))

/* 全局累计发送字节计数器, 用于节流控制 */
static int tx_byte_count = 0;

/*
 * uartinit: 16550A UART 硬件底层初始化
 */
void
uartinit(void)
{
  // 1. 关闭所有串口中断(轮询模式)
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
  uartinit();
}
