/* mx_loader_launch, then the end without the restart: the launched program
   starts after mxlink's restart, before mxlink's own clients have called
   the IOP, so what they make then comes after the program's start. Its end
   must leave the kernel working (the next run shows it).
   vm26_launch.elf: it launches itself with "child".
   vm26_launch.elf launch <swap or -> <path> [args]: launches that, as the
   shell starts a game (mxlink/shell/games.c).
   vm26_launch.elf read <path>: reads the file through (a check after the
   above: the shell's start after a game hung there). */
#include <mx/debug.h>
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/string.h>

static char args[512];

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    mx_printf("vm26_launch: the launched program runs\n");
    return 0;
  }
  if (argc > 2 && !strcmp(argv[1], "read")) {
    static u8 buf[16384] __attribute__((aligned(64)));
    int fd = mx_fio_open(argv[2], MX_FIO_RDONLY);
    mx_printf("vm26_launch: open %s: %d\n", argv[2], fd);
    if (fd < 0)
      return 1;
    int total = 0, n;
    while ((n = mx_fio_read(fd, buf, sizeof buf)) > 0)
      total += n;
    mx_fio_close(fd);
    mx_printf("vm26_launch: read %d bytes (last %d)\n", total, n);
    return n < 0;
  }
  int first = 0;
  const char *swap = 0;
  if (argc > 3 && !strcmp(argv[1], "launch")) {
    swap = strcmp(argv[2], "-") ? argv[2] : 0;
    first = 3;
  }
  u32 n = 0;
  int count = 0;
  for (int i = first; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      return 1;
    memcpy(args + n, argv[i], l);
    n += l;
    count++;
  }
  if (!first) {
    memcpy(args + n, "child", 6);
    n += 6;
    count++;
  }
  mx_printf("vm26_launch: launching %s (%d words, swap %s)\n", args, count, swap ? swap : "-");
  mx_loader_launch(count, args, n, swap);
  mx_printf("vm26_launch: no launch\n");
  return 1;
}
