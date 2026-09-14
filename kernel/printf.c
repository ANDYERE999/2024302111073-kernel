#include <stdarg.h>
#include "types.h"
#include "riscv.h"

void consputc(int c);

static char digits[] = "0123456789abcdef";

/*
 * printint: 整数格式化打印
 * xx: 待打印数值
 * base: 进制(10 或 16)
 * sign: 是否作为有符号数处理(1=有符号, 0=无符号)
 */
static void
printint(long long xx, int base, int sign)
{
  char buf[32];
  int i = 0;
  unsigned long long x;

  if (sign && xx < 0) {
    x = (unsigned long long)(-xx);
    consputc('-');
  } else {
    x = (unsigned long long)xx;
  }

  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  while (--i >= 0) {
    consputc(buf[i]);
  }
}

/*
 * printptr: 指针地址打印 (格式: 0x...)
 */
static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4) {
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
  }
}

/*
 * printf: 最小内核格式化输出
 * 支持格式占位符:
 *   %d  - 有符号十进制整数
 *   %u  - 无符号十进制整数
 *   %x  - 无符号十六进制(小写, 无前导零)
 *   %p  - 指针地址
 *   %s  - 字符串(对 NULL 指针安全防护输出 "(null)")
 *   %c  - 字符
 *   %%  - 转义百分号
 */
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, c;
  char *s;

  if (fmt == 0)
    return 0;

  va_start(ap, fmt);
  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      consputc(c);
      continue;
    }

    i++;
    c = fmt[i] & 0xff;
    if (c == 0)
      break;

    switch (c) {
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'u':
      printint(va_arg(ap, uint), 10, 0);
      break;
    case 'x':
      printint(va_arg(ap, uint), 16, 0);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 'c':
      consputc(va_arg(ap, int));
      break;
    case 's':
      s = va_arg(ap, char *);
      if (s == 0)
        s = "(null)";
      while (*s) {
        consputc(*s++);
      }
      break;
    case '%':
      consputc('%');
      break;
    default:
      // 未知转义字符原样回显
      consputc('%');
      consputc(c);
      break;
    }
  }
  va_end(ap);

  return 0;
}

/*
 * panic: 内核异常崩溃输出
 */
void
panic(char *s)
{
  printf("panic: %s\n", s);
  for (;;)
    ;
}
