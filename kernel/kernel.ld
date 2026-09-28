/* ============ 预置支撑代码 ============
 * 说明: 加载地址与段排布规则
 * ======================================== */
OUTPUT_ARCH(riscv)
ENTRY(_entry)

BASE_ADDRESS = 0x80000000;

SECTIONS
{
  . = BASE_ADDRESS;

  .text : {          /* 代码段:entry.S 的第一段指令就在这里开头 */
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(etext = .);
  }

  .rodata : {        /* 只读数据:字符串常量等 */
    . = ALIGN(16);
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  }

  .data : {          /* 已初始化数据 */
    . = ALIGN(16);
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  .bss : {           /* 未初始化数据:启动时由内核自行清零 */
    . = ALIGN(16);
    *(.bss .bss.*)
    *(.sbss*)
  }

  PROVIDE(end = .);
  /* 内核结束符号:lab3 的 kalloc 将从这里开始分配物理页 */
  kernel_end = .;
}
