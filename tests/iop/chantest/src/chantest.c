/* Sends three SIF commands with cid 7 to the EE (tests/ps2/k37_iopchan.c):
   each a word, its number 1-3, and a text. Stays resident: the packets are
   its memory until their DMA is done. */
#include <irx.h>
#include <loadcore.h>
#include <sifcmd.h>

IRX_ID("chantest", 1, 0);

#define CID 7

struct pkt {
  SifCmdHeader_t h;
  u32 n;
  char text[12];
};
static struct pkt pkts[3] __attribute__((aligned(16)));

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  for (u32 i = 0; i < 3; i++) {
    struct pkt *p = &pkts[i];
    p->n = i + 1;
    const char *t = "chantest";
    for (u32 k = 0; k < sizeof p->text; k++)
      p->text[k] = k < 8 ? t[k] : 0;
    sceSifSendCmd(CID, p, sizeof *p, 0, 0, 0);
  }
  return MODULE_RESIDENT_END;
}
