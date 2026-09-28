#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "course_sid.h"
#include "defs.h"

/*
 * 将无符号整数转换为字符串
 */
static void
uint64_to_str(uint64 val, char *dst, int base)
{
  char buf[32];
  int i = 0;
  static const char digits[] = "0123456789abcdef";

  if (val == 0) {
    *dst++ = '0';
    *dst = '\0';
    return;
  }

  while (val > 0) {
    buf[i++] = digits[val % base];
    val /= base;
  }

  while (--i >= 0) {
    *dst++ = buf[i];
  }
  *dst = '\0';
}

/*
 * 字符串拷贝与拼接辅助例程
 */
static char *
str_append(char *dst, const char *src)
{
  while (*src) {
    *dst++ = *src++;
  }
  *dst = '\0';
  return dst;
}

/*
 * 构造 Banner 正文字符串: "OSLAB1 sid=<学号> mod97=0x<十六进制>\n"
 */
static void
build_banner_body(char *buf)
{
  char *p = buf;
  p = str_append(p, "OSLAB1 sid=");
  uint64_to_str(COURSE_SID, p, 10);
  while (*p)
    p++;

  p = str_append(p, " mod97=0x");
  uint64_to_str((COURSE_SID % 97), p, 16);
  while (*p)
    p++;

  *p++ = '\n';
  *p = '\0';
}

/*
 * 计算 ASCII 码校验和 (对正文字符串所有字节累加求和)
 */
static int
calc_ascii_checksum(const char *s)
{
  int sum = 0;
  while (*s) {
    sum += (unsigned char)(*s);
    s++;
  }
  return sum;
}

/*
 * print_banner: 根据 LAB1_BANNER_PROTOCOL 输出个性化 Banner 及自检序列
 */
static void
print_banner(void)
{
  char body[128];
  build_banner_body(body);

#if LAB1_BANNER_PROTOCOL == 0
  // 协议 0: 原始明文 + 换行
  printf("%s", body);

#elif LAB1_BANNER_PROTOCOL == 1
  // 协议 1: 每个字节后固定紧跟一个 '.'
  for (int i = 0; body[i] != '\0'; i++) {
    printf("%c.", body[i]);
  }

#elif LAB1_BANNER_PROTOCOL == 2
  // 协议 2: 正文输出后, 最末段附带 ASCII 十进制数字校验和 [chk=xxxx], 其后不再有正文
  int chk = calc_ascii_checksum(body);
  printf("%s[chk=%d]", body, chk);

#endif
}

/*
 * test_printf_boundaries: printf 边界测试套件
 * 供答辩现场演示或验证 printf 功能完备性
 */
void
test_printf_boundaries(void)
{
  printf("\n=== printf Boundary Tests ===\n");
  printf("1. Zero: %d\n", 0);
  printf("2. Negative: %d\n", -12345);
  printf("3. Min Int: %d\n", -2147483648);
  printf("4. Max Int: %d\n", 2147483647);
  printf("5. Hex lower: 0x%x\n", 0x39);
  printf("6. Hex zero: 0x%x\n", 0);
  printf("7. Empty string: [%s]\n", "");
  printf("8. Null string: [%s]\n", (char *)0);
  printf("9. Char & Percent: %c %%\n", 'A');
  printf("10. Long string: %s\n", "The quick brown fox jumps over the lazy dog.");
  printf("=== Boundary Tests Done ===\n");
}

/*
 * main(): Supervisor 模式下的内核主入口
 */
void
main(void)
{
  // 1. 初始化控制台与底层 UART 16550 驱动
  consoleinit();
  printfinit();

  // 2. 打印个性化 Banner 与自检序列 (严格匹配 expect_banner.txt)
  print_banner();

#ifdef RUN_TESTS
  // 若定义了 RUN_TESTS 宏, 则执行 printf 边界测试
  test_printf_boundaries();
#endif

  // 3. 内存管理与虚拟地址分页初始化
  kinit();         // 物理页面分配器
  kvminit();       // 内核页表创建
  kvminithart();   // 开启 Sv39 硬件地址翻译

  // 4. 进程与中断机制初始化
  procinit();      // 进程表与内核栈映射
  trapinit();      // 陷阱锁初始化
  trapinithart();  // 安装内核陷阱向量 kernelvec
  plicinit();      // PLIC 中断控制器硬件配置
  plicinithart();  // 开启当前 CPU 核心的 PLIC 外部中断响应

  // 5. 加载并装配第一个用户态进程 Shell (sh)
  userinit();

  __sync_synchronize();

  // 6. 正式进入进程调度循环，切入用户态
  scheduler();
}
