#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "course_sid.h"

/* 声明 C 语言主入口函数 */
void main(void);
void timerinit(void);

/*
 * start(): entry.S 建立初始内核栈后在 M 态调用的初始化例程
 * 负责完成从 Machine 模式降权切换到 Supervisor 模式的关键配置清单
 */
void
start()
{
  // 1. 配置 mstatus.MPP 为 S 模式 (Supervisor)
  // 使得执行 mret 指令时 CPU 能正确降权进入 S 态
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // 2. 将 mepc 目标地址指向 main 函数
  // mret 执行后硬件将把 PC 设置为 mepc 的值
  w_mepc((uint64)main);

  // 3. 禁用地址翻译与保护(Bare 模式, satp = 0)
  // 启动初期尚未建立内核页表, 此时虚拟地址直接等于物理地址
  w_satp(0);

  // 4. 中断与异常委托配置 (Machine Delegation)
  // 将所有中断与异常委托给 Supervisor 态处理
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  // 开启 S 态外部中断 (SIE_SEIE) 与定时器中断 (SIE_STIE)
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  // 5. PMP (物理内存保护) 配置: 必选前置条件
  // 授予 Supervisor 态访问全部物理内存的读写执行权限(NAPOT模式, R/W/X=1)
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // 6. 配置硬件定时器中断 (sstc 拓展)
  timerinit();

  // 7. 保持当前 CPU 的 hartid 于 tp 寄存器中
  w_tp(r_mhartid());

  // 8. 执行 mret, 硬件原子完成特权级降入 S 态并跳转至 main()
  asm volatile("mret");
}

/*
 * timerinit: 配置 RISC-V 定时器中断
 */
void
timerinit()
{
  // 启用 sstc 扩展中的 stimecmp
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);

  // 允许 S 态直接使用 stimecmp 与 time 寄存器
  w_mcounteren(r_mcounteren() | 2);

  // 设定初次时钟中断发生时间 (~0.1s 后)
  w_stimecmp(r_time() + 1000000);
}
