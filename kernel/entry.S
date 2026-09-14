#include "course_sid.h"

/*
 * entry.S: RISC-V 裸机第一条指令入口
 * 链接脚本 kernel.ld 将 _entry 放置在 0x80000000
 */

.section .text
.globl _entry
_entry:
    # 1. 立即关闭机器模式所有中断(mie 置 0)
    # 防止在栈未初始化、异常向量未设置前响应外部/时钟中断导致系统崩溃
    csrw mie, zero

    # 2. 从核(mhartid != 0)挂起
    # 多核并发时避免各核心争用初始内核栈与未初始化的全局数据
    csrr tp, mhartid
    bnez tp, park

    # 3. 设置初始内核栈
    # 栈由高向低生长, 栈顶设为 bootstack + LAB1_STACK_KB * 1024
    la   sp, bootstack
    li   t0, LAB1_STACK_KB * 1024
    add  sp, sp, t0

    # 4. 配置机器模式初始陷阱向量基址寄存器(mtvec)
    # 标号 park_trap 严格 4 字节对齐, 避免 QEMU 静默丢弃写入
    la   t1, park_trap
    csrw mtvec, t1

    # 5. 跳转至 C 语言入口 start()
    call start

    # 兜底自旋
park:
    wfi
    j park

    # mtvec 要求必须 4 字节对齐(低 2 位为模式位)
.balign 4
park_trap:
    wfi
    j park_trap

.section .bss
.balign 16
.globl bootstack
bootstack:
    # 初始内核栈空间: 12KB, 必须符合个性化参数 LAB1_STACK_KB
    .space LAB1_STACK_KB * 1024
