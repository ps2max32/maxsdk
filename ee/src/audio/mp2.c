/* MPEG audio layer II (<mx/mp2.h>; ISO 11172-3, 2.4.3 and annex 3-A, 3-B).
   The tables below are the standard's (the synthesis window D and the bit
   allocation tables B.2, in the compact form of pl_mpeg, MIT, by Dominic
   Szablewski, from where the numbers were taken); the code is maxsdk's.

   Integers throughout: the EE's FPU is single precision, and its multiplier
   gives 32 x 32 to 64 bits. A subband sample has 17 bits of fraction. */

#include <mx/mp2.h>
#include <mx/string.h>

/* D[i] x 65536 */
static const s32 window[512] = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3,
    -3, -4, -4, -5, -5, -6, -7, -7, -8, -9, -10, -11,
    -13, -14, -16, -17, -19, -21, -24, -26, -29, -31, -35, -38,
    -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183,
    -190, -196, -202, -208, 213, 218, 222, 225, 227, 228, 228, 227,
    224, 221, 215, 208, 200, 189, 177, 163, 146, 127, 106, 83,
    57, 29, -2, -36, -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919, -991, -1064, -1137, -1210,
    -1283, -1356, -1428, -1498, -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063, 2037, 2000, 1952, 1893,
    1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351,
    -3705, -4063, -4425, -4788, -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597,
    -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
    -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082, 70, -998, -2122, -3300,
    -4533, -5818, -7154, -8540, -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189,
    -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640, -37489, -39336, -41176, -43006,
    -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908,
    -74313, -74630, -74856, -74992, 75038, 74992, 74856, 74630, 74313, 73908, 73415, 72835,
    72169, 71420, 70590, 69679, 68692, 67629, 66494, 65290, 64019, 62684, 61289, 59838,
    58333, 56778, 55178, 53534, 51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336,
    37489, 35640, 33791, 31947, 30112, 28289, 26482, 24694, 22929, 21189, 19478, 17799,
    16155, 14548, 12980, 11455, 9975, 8540, 7154, 5818, 4533, 3300, 2122, 998,
    -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959, 6574, 7134, 7640, 8092,
    8492, 8840, 9139, 9389, 9592, 9750, 9863, 9935, 9966, 9959, 9916, 9838,
    9727, 9585, 9416, 9219, 8998, 8755, 8491, 8209, 7910, 7597, 7271, 6935,
    6589, 6237, 5879, 5517, 5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663,
    2330, 2006, 1692, 1388, 1095, 814, 545, 288, 45, -185, -402, -605,
    -794, -970, -1131, -1280, -1414, -1535, -1644, -1739, -1822, -1893, -1952, -2000,
    2037, 2063, 2080, 2087, 2085, 2075, 2057, 2032, 2001, 1962, 1919, 1870,
    1817, 1759, 1698, 1634, 1567, 1498, 1428, 1356, 1283, 1210, 1137, 1064,
    991, 919, 848, 779, 711, 645, 581, 519, 459, 401, 347, 294,
    244, 197, 153, 111, 72, 36, 2, -29, -57, -83, -106, -127,
    -146, -163, -177, -189, -200, -208, -215, -221, -224, -227, -228, -228,
    -227, -225, -222, -218, 213, 208, 202, 196, 190, 183, 176, 169,
    161, 154, 147, 139, 132, 125, 117, 111, 104, 97, 91, 85,
    79, 73, 68, 63, 58, 53, 49, 45, 41, 38, 35, 31,
    29, 26, 24, 21, 19, 17, 16, 14, 13, 11, 10, 9,
    8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 2, 2,
    2, 2, 1, 1, 1, 1, 1, 1,
};

/* For the DCT of 32 points by halves (Lee): 1 / (2 cos((2k + 1) pi / 2N)),
   20 bits of fraction, for N = 32 (16 of them), 16, 8, 4, 2 */
static const s32 lee[31] = {
    524920, 530025, 540486, 556839, 579971, 611252, 652743, 707588,
    780703, 880122, 1019812, 1226247, 1556259, 2157740, 3573136, 10684998,
    526825, 547880, 594484, 678242, 826440, 1112201, 1806117, 5348942,
    534559, 630556, 943693, 2687412, 567485, 1370031, 741455,
};

static const u16 rates[3] = {44100, 48000, 32000};
static const u16 kbits[14] = {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
/* 2^(1 - i/3) for i = 0, 1, 2, 24 bits of fraction */
static const s32 scale_base[3] = {0x02000000, 0x01965FEA, 0x01428A30};

/* Which allocation table: by the bit rate (for one channel, for two), then
   by the sample rate: its number (bit 6: the high-rate tables B.2a, B.2b)
   and its subbands */
static const u8 class_of[2][14] = {{0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2},
                                   {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2}};
static const u8 table_of[3][3] = {{8, 8, 12}, {27 | 64, 27 | 64, 27 | 64}, {30 | 64, 27 | 64, 30 | 64}};
/* by table (low rate, high rate) and subband: the allocation's bits (high
   nibble) and its row below */
static const u8 alloc_of[2][32] = {
    {0x44, 0x44, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34},
    {0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x31, 0x31, 0x31, 0x31,
     0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}};
/* by row and allocation: the quantizer (0: no samples) */
static const u8 quant_of[5][16] = {{0, 1, 2, 17},
                                   {0, 1, 2, 3, 4, 5, 6, 17},
                                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
                                   {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
                                   {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17}};
/* a quantizer: its levels; grouped: three samples in one code of bits bits,
   else each in bits bits */
static const struct {
  u16 levels;
  u8 group, bits;
} quant[17] = {{3, 1, 5},     {5, 1, 7},     {7, 0, 3},     {9, 1, 10},    {15, 0, 4},    {31, 0, 5},
               {63, 0, 6},    {127, 0, 7},   {255, 0, 8},   {511, 0, 9},   {1023, 0, 10}, {2047, 0, 11},
               {4095, 0, 12}, {8191, 0, 13}, {16383, 0, 14}, {32767, 0, 15}, {65535, 0, 16}};

/* 1 / levels, 32 bits of fraction */
#define INV(l) ((u32)(0x100000000ull / (l)))
static const u32 inv_levels[17] = {INV(3),    INV(5),    INV(7),    INV(9),    INV(15),   INV(31),
                                   INV(63),   INV(127),  INV(255),  INV(511),  INV(1023), INV(2047),
                                   INV(4095), INV(8191), INV(16383), INV(32767), INV(65535)};

struct bits {
  const u8 *p;
  u32 pos;
};
static u32 get(struct bits *b, u32 n) {
  u32 v = 0;
  for (; n; n--, b->pos++)
    v = v << 1 | (b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1);
  return v;
}

void mx_mp2_init(struct mx_mp2 *d) { memset(d, 0, sizeof *d); }

/* X[n] = sum of x[k] cos((2k + 1) n pi / 2N), in place: the sums and the
   scaled differences of the two halves each by the same of half the size */
static void dct(s32 *x, u32 n, const s32 *co) {
  s32 a[16], b[16];
  u32 h = n / 2;
  if (n == 1)
    return;
  for (u32 k = 0; k < h; k++) {
    a[k] = x[k] + x[n - 1 - k];
    b[k] = (s32)(((s64)(x[k] - x[n - 1 - k]) * co[k]) >> 20);
  }
  dct(a, h, co + h);
  dct(b, h, co + h);
  for (u32 k = 0; k + 1 < h; k++) {
    x[2 * k] = a[k];
    x[2 * k + 1] = b[k] + b[k + 1];
  }
  x[n - 2] = a[h - 1];
  x[n - 1] = b[h - 1];
}

/* 32 subband samples to 32 samples of sound (3-A.2): the matrixing by the
   DCT (cos((16 + i)(2k + 1) pi / 64) is +-X of another index), then the
   window over the last 16 of its results. out: every second s16. */
static void synth(struct mx_mp2 *d, u32 ch, s32 *s, s16 *out) {
  s32 *v = d->v[ch];
  u32 at = d->pos;
  dct(s, 32, lee);
  for (u32 i = 0; i < 16; i++)
    v[(at + i) & 1023] = s[16 + i];
  v[(at + 16) & 1023] = 0;
  for (u32 i = 17; i < 48; i++)
    v[(at + i) & 1023] = -s[48 - i];
  v[(at + 48) & 1023] = -s[0];
  for (u32 i = 49; i < 64; i++)
    v[(at + i) & 1023] = -s[i - 48];
  for (u32 j = 0; j < 32; j++) {
    s64 sum = 0;
    for (u32 i = 0; i < 8; i++) {
      sum += (s64)v[(at + i * 128 + j) & 1023] * window[i * 64 + j];
      sum += (s64)v[(at + i * 128 + 96 + j) & 1023] * window[i * 64 + 32 + j];
    }
    s32 o = (s32)((sum + (1 << 17)) >> 18);
    out[j * 2] = (s16)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
  }
}

u32 mx_mp2_decode(struct mx_mp2 *d, const u8 *data, u32 bytes, s16 *out, u32 *frames, u32 *rate) {
  *frames = 0;
  /* the header: 11 bits of sync, MPEG-1 (3), layer II (2) */
  u32 skip = 0;
  while (skip + 4 <= bytes && !(data[skip] == 0xFF && (data[skip + 1] & 0xFE) == 0xFC))
    skip++;
  if (skip)
    return skip;
  if (bytes < 4)
    return 0;
  u32 crc = !(data[1] & 1), br = data[2] >> 4, sr = data[2] >> 2 & 3, pad = data[2] >> 1 & 1;
  u32 mode = data[3] >> 6, ext = data[3] >> 4 & 3;
  if (br < 1 || br > 14 || sr > 2)
    return 1; /* no frame after all: on from the next byte */
  u32 size = 144000u * kbits[br - 1] / rates[sr] + pad;
  if (bytes < size)
    return 0;
  *rate = rates[sr];
  struct bits b = {data, 32 + (crc ? 16 : 0)};
  u32 nch = mode == 3 ? 1 : 2, bound = mode == 1 ? (ext + 1) * 4 : mode == 3 ? 0 : 32;
  u32 t = table_of[class_of[nch - 1][br - 1]][sr], sblimit = t & 63, high = t >> 6;
  if (mode == 3)
    bound = sblimit; /* one channel: each subband's bits once */
  if (bound > sblimit)
    bound = sblimit;

  u8 alloc[2][32], scfsi[2][32], sf[2][32][3];
  memset(alloc, 0, sizeof alloc);
  for (u32 sb = 0; sb < sblimit; sb++) {
    u32 e = alloc_of[high][sb];
    for (u32 ch = 0; ch < (sb < bound ? nch : 1); ch++)
      alloc[ch][sb] = quant_of[e & 15][get(&b, e >> 4)];
    if (sb >= bound)
      alloc[1][sb] = alloc[0][sb];
  }
  for (u32 sb = 0; sb < sblimit; sb++)
    for (u32 ch = 0; ch < nch; ch++)
      if (alloc[ch][sb])
        scfsi[ch][sb] = (u8)get(&b, 2);
  /* three scale factors a subband: how many are sent says scfsi */
  for (u32 sb = 0; sb < sblimit; sb++)
    for (u32 ch = 0; ch < nch; ch++)
      if (alloc[ch][sb]) {
        u8 *f = sf[ch][sb];
        f[0] = (u8)get(&b, 6);
        switch (scfsi[ch][sb]) {
        case 0:
          f[1] = (u8)get(&b, 6), f[2] = (u8)get(&b, 6);
          break;
        case 1:
          f[1] = f[0], f[2] = (u8)get(&b, 6);
          break;
        case 2:
          f[1] = f[2] = f[0];
          break;
        default:
          f[1] = f[2] = (u8)get(&b, 6);
        }
      }

  /* 12 times three samples a subband; the scale factor changes every 4 */
  d->pos &= 1023;
  for (u32 gr = 0; gr < 12; gr++) {
    s32 s[2][3][32];
    memset(s, 0, sizeof s);
    for (u32 sb = 0; sb < sblimit; sb++)
      for (u32 ch = 0; ch < (sb < bound ? nch : 1); ch++) {
        u32 q = alloc[ch][sb];
        if (!q)
          continue;
        u32 L = quant[q - 1].levels, n = quant[q - 1].bits, val[3];
        if (quant[q - 1].group) {
          u32 c = get(&b, n);
          val[0] = c % L, c /= L;
          val[1] = c % L, val[2] = c / L;
        } else {
          val[0] = get(&b, n), val[1] = get(&b, n), val[2] = get(&b, n);
        }
        /* a sample is (2 val + 1 - L) / L of the scale factor; above the
           bound both channels have the one code, each its own factor */
        u32 inv = inv_levels[q - 1];
        for (u32 c2 = ch; c2 < (sb < bound ? ch + 1 : nch); c2++) {
          u32 i = sf[c2][sb][gr / 4];
          s32 scale = i < 63 ? scale_base[i % 3] >> (i / 3) : 0;
          for (u32 k = 0; k < 3; k++) {
            /* 30 bits of fraction, times the factor's 24, down to 17 */
            s32 frac = (s32)(((s64)((s32)(2 * val[k] + 1) - (s32)L) * inv) >> 2);
            s[c2][k][sb] = (s32)(((s64)frac * scale + ((s64)1 << 36)) >> 37);
          }
        }
      }
    for (u32 k = 0; k < 3; k++) {
      d->pos = (d->pos - 64) & 1023;
      s16 *o = out + (gr * 3 + k) * 64;
      for (u32 ch = 0; ch < nch; ch++)
        synth(d, ch, s[ch][k], o + ch);
      if (nch == 1)
        for (u32 j = 0; j < 32; j++)
          o[j * 2 + 1] = o[j * 2];
    }
  }
  *frames = MX_MP2_SAMPLES;
  return size;
}
