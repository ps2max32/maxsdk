/* Still pictures (<mx/pic.h>): the format by its first bytes, the memory. */

#include <mx/mem.h>
#include <mx/pic.h>
#include <mx/sif.h>
#include <mx/string.h>

static int is_jpeg(const u8 *d, u32 n) { return n > 3 && d[0] == 0xFF && d[1] == 0xD8; }
static int is_png(const u8 *d, u32 n) { return n > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G'; }

int mx_pic_size(const void *data, u32 bytes, u16 *width, u16 *height) {
  if (is_jpeg(data, bytes))
    return mx_jpeg_size(data, bytes, width, height);
  if (is_png(data, bytes))
    return mx_png_size(data, bytes, width, height);
  return MX_PIC_FORMAT;
}

int mx_pic_decode(const void *data, u32 bytes, struct mx_pic *p) {
  memset(p, 0, sizeof *p);
  int r = mx_pic_size(data, bytes, &p->width, &p->height);
  if (r)
    return r;
  if (!p->width || !p->height)
    return MX_PIC_DATA;
  p->pixels = mx_dma_alloc((u32)p->width * p->height * 4);
  if (!p->pixels)
    return MX_PIC_MEMORY;
  r = is_jpeg(data, bytes) ? mx_jpeg_decode(data, bytes, p->pixels, &p->alpha)
                           : mx_png_decode(data, bytes, p->pixels, &p->alpha);
  if (r)
    mx_pic_free(p);
  else
    mx_dcache_writeback(p->pixels, (u32)p->width * p->height * 4);
  return r;
}

int mx_pic_load(const char *path, struct mx_pic *p) {
  memset(p, 0, sizeof *p);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return MX_PIC_FILE;
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  u8 *file = size > 0 ? mx_heap_alloc(mx_heap_paged, (u32)size, 64) : 0;
  int r = size <= 0 ? MX_PIC_FILE : !file ? MX_PIC_MEMORY : MX_PIC_OK;
  for (int got = 0; !r && got < size;) {
    int n = mx_fio_read(fd, file + got, (u32)(size - got));
    if (n <= 0)
      r = MX_PIC_FILE;
    got += n;
  }
  mx_fio_close(fd);
  if (!r)
    r = mx_pic_decode(file, (u32)size, p);
  if (file)
    mx_heap_free(mx_heap_paged, file);
  return r;
}

void mx_pic_free(struct mx_pic *p) {
  if (p->pixels)
    mx_dma_free(p->pixels);
  p->pixels = 0;
}
