/*
 * Copyright (C) 1999-2001, 2005 Free Software Foundation, Inc.
 * This file is part of the GNU LIBICONV Library.
 *
 * The GNU LIBICONV Library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * The GNU LIBICONV Library is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the GNU LIBICONV Library; see the file COPYING.LIB.
 * If not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * GB18030 two-byte extension
 */

static const unsigned short gb18030ext_2uni_pagea9[13] = {
  /* 0xa9 */
  0x303e, 0x2ff0, 0x2ff1, 0x2ff2, 0x2ff3, 0x2ff4, 0x2ff5, 0x2ff6,
  0x2ff7, 0x2ff8, 0x2ff9, 0x2ffa, 0x2ffb,
};
static const unsigned short gb18030ext_2uni_pagefe[96] = {
  /* 0xfe */
  0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd,
  0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd,
  0x2e81, 0xe816, 0xe817, 0xe818, 0x2e84, 0x3473, 0x3447, 0x2e88,
  0x2e8b, 0xe81e, 0x359e, 0x361a, 0x360e, 0x2e8c, 0x2e97, 0x396e,
  0x3918, 0xe826, 0x39cf, 0x39df, 0x3a73, 0x39d0, 0xe82b, 0xe82c,
  0x3b4e, 0x3c6e, 0x3ce0, 0x2ea7, 0xe831, 0xe832, 0x2eaa, 0x4056,
  0x415f, 0x2eae, 0x4337, 0x2eb3, 0x2eb6, 0x2eb7, 0xe83b, 0x43b1,
  0x43ac, 0x2ebb, 0x43dd, 0x44d6, 0x4661, 0x464c, 0xe843, 0x4723,
  0x4729, 0x477c, 0x478d, 0x2eca, 0x4947, 0x497a, 0x497d, 0x4982,
  0x4983, 0x4985, 0x4986, 0x499f, 0x499b, 0x49b7, 0x49b6, 0xe854,
  0xe855, 0x4ca3, 0x4c9f, 0x4ca0, 0x4ca1, 0x4c77, 0x4ca2, 0x4d13,
  0x4d14, 0x4d15, 0x4d16, 0x4d17, 0x4d18, 0x4d19, 0x4dae, 0xe864,
};

static int
gb18030ext_mbtowc (conv_t conv, ucs4_t *pwc, const unsigned char *s, int n)
{
  unsigned char c1 = s[0];
  if ((c1 == 0xa2) || (c1 >= 0xa4 && c1 <= 0xa9) || (c1 == 0xd7) || (c1 == 0xfe)) {
    if (n >= 2) {
      unsigned char c2 = s[1];
      if ((c2 >= 0x40 && c2 < 0x7f) || (c2 >= 0x80 && c2 < 0xff)) {
        unsigned int i = 190 * (c1 - 0x81) + (c2 - (c2 >= 0x80 ? 0x41 : 0x40));
        unsigned short wc = 0xfffd;
        switch (c1) {
          case 0xa2:
            if (i >= 6376 && i <= 6381) /* 0xA2AB..0xA2B0 */
              wc = 0xe766 + (i - 6376);
            else if (i == 6432) /* 0xA2E3 */
              wc = 0x20ac;
            else if (i == 6433) /* 0xA2E4 */
              wc = 0xe76d;
            else if (i >= 6444 && i <= 6445) /* 0xA2EF..0xA2F0 */
              wc = 0xe76e + (i - 6444);
            else if (i >= 6458 && i <= 6459) /* 0xA2FD..0xA2FE */
              wc = 0xe770 + (i - 6458);
            break;
          case 0xa4:
            if (i >= 6829 && i <= 6839) /* 0xA4F4..0xA4FE */
              wc = 0xe772 + (i - 6829);
            break;
          case 0xa5:
            if (i >= 7022 && i <= 7029) /* 0xA5F7..0xA5FE */
              wc = 0xe77d + (i - 7022);
            break;
          case 0xa6:
            if (i >= 7150 && i <= 7157) /* 0xA6B9..0xA6C0 */
              wc = 0xe785 + (i - 7150);
            else if (i >= 7182 && i <= 7190) /* 0xA6D9..0xA6DF */
              wc = 0xe78d + (i - 7182);
            else if (i >= 7201 && i <= 7202) /* 0xA6EC..0xA6ED */
              wc = 0xe794 + (i - 7201);
            else if (i == 7208) /* 0xA6F3 */
              wc = 0xe796;
            else if (i >= 7211 && i <= 7219) /* 0xA6F6..0xA6FE */
              wc = 0xe797 + (i - 7211);
            break;
          case 0xa7:
            if (i >= 7349 && i <= 7363) /* 0xA7C2..0xA7D0 */
              wc = 0xe7a0 + (i - 7349);
            else if (i >= 7397 && i <= 7409) /* 0xA7F2..0xA7FE */
              wc = 0xe7af + (i - 7397);
            break;
          case 0xa8:
            if (i >= 7495 && i <= 7505) /* 0xA896..0xA8A0 */
              wc = 0xe7bc + (i - 7495);
            else if (i == 7533) /* 0xA8BC */
              wc = 0xe7c7;
            else if (i == 7536) /* 0xA8BF */
              wc = 0x01f9;
            else if (i >= 7538 && i <= 7541) /* 0xA8C1..0xA8C4 */
              wc = 0xe7c9 + (i - 7538);
            else if (i >= 7579 && i <= 7599) /* 0xA8EA..0xA8FE */
              wc = 0xe7cd + (i - 7579);
            break;
          case 0xa9:
            if (i == 7624) /* 0xA958 */
              wc = 0xe7e2;
            else if (i == 7627) /* 0xA95B */
              wc = 0xe7e3;
            else if (i >= 7629 && i <= 7631) /* 0xA95D..0xA95F */
              wc = 0xe7e4 + (i - 7629);
            else if (i >= 7672 && i < 7685) /* 0xA989..0xA995 */
              wc = gb18030ext_2uni_pagea9[i-7672];
            else if (i >= 7686 && i <= 7698) /* 0xA997..0xA9A3 */
              wc = 0xe7f4 + (i - 7686);
            else if (i >= 7775 && i <= 7789) /* 0xA9F0..0xA9FE */
              wc = 0xe801 + (i - 7775);
            break;
          case 0xd7:
            if (i >= 16525 && i <= 16529) /* 0xD7FA..0xD7FE */
              wc = 0xe810 + (i - 16525);
            break;
          case 0xfe:
            if (i < 23846)
              wc = gb18030ext_2uni_pagefe[i-23750];
            break;
          default:
            break;
        }
        if (wc != 0xfffd) {
          *pwc = (ucs4_t) wc;
          return 2;
        }
      }
      return RET_ILSEQ;
    }
    return RET_TOOFEW(0);
  }
  return RET_ILSEQ;
}

static const unsigned short gb18030ext_page2e[80] = {
  0x0000, 0xfe50, 0x0000, 0x0000, 0xfe54, 0x0000, 0x0000, 0x0000, /*0x80-0x87*/
  0xfe57, 0x0000, 0x0000, 0xfe58, 0xfe5d, 0x0000, 0x0000, 0x0000, /*0x88-0x8f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe5e, /*0x90-0x97*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x98-0x9f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe6b, /*0xa0-0xa7*/
  0x0000, 0x0000, 0xfe6e, 0x0000, 0x0000, 0x0000, 0xfe71, 0x0000, /*0xa8-0xaf*/
  0x0000, 0x0000, 0x0000, 0xfe73, 0x0000, 0x0000, 0xfe74, 0xfe75, /*0xb0-0xb7*/
  0x0000, 0x0000, 0x0000, 0xfe79, 0x0000, 0x0000, 0x0000, 0x0000, /*0xb8-0xbf*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xc0-0xc7*/
  0x0000, 0x0000, 0xfe84, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xc8-0xcf*/
};
static const unsigned short gb18030ext_page2f[16] = {
  0xa98a, 0xa98b, 0xa98c, 0xa98d, 0xa98e, 0xa98f, 0xa990, 0xa991, /*0xf0-0xf7*/
  0xa992, 0xa993, 0xa994, 0xa995, 0x0000, 0x0000, 0x0000, 0x0000, /*0xf8-0xff*/
};
static const unsigned short gb18030ext_page34[56] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe56, /*0x40-0x47*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x48-0x4f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x50-0x57*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x58-0x5f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x60-0x67*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x68-0x6f*/
  0x0000, 0x0000, 0x0000, 0xfe55, 0x0000, 0x0000, 0x0000, 0x0000, /*0x70-0x77*/
};
static const unsigned short gb18030ext_page36[24] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe5c, 0x0000, /*0x08-0x0f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x10-0x17*/
  0x0000, 0x0000, 0xfe5b, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x18-0x1f*/
};
static const unsigned short gb18030ext_page39[24] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe62, /*0xc8-0xcf*/
  0xfe65, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xd0-0xd7*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe63, /*0xd8-0xdf*/
};
static const unsigned short gb18030ext_page43[56] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0xfe78, 0x0000, 0x0000, 0x0000, /*0xa8-0xaf*/
  0x0000, 0xfe77, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xb0-0xb7*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xb8-0xbf*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xc0-0xc7*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xc8-0xcf*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xd0-0xd7*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe7a, 0x0000, 0x0000, /*0xd8-0xdf*/
};
static const unsigned short gb18030ext_page46[32] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0xfe7d, 0x0000, 0x0000, 0x0000, /*0x48-0x4f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x50-0x57*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x58-0x5f*/
  0x0000, 0xfe7c, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x60-0x67*/
};
static const unsigned short gb18030ext_page47_1[16] = {
  0x0000, 0x0000, 0x0000, 0xfe80, 0x0000, 0x0000, 0x0000, 0x0000, /*0x20-0x27*/
  0x0000, 0xfe81, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x28-0x2f*/
};
static const unsigned short gb18030ext_page47_2[24] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0xfe82, 0x0000, 0x0000, 0x0000, /*0x78-0x7f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x80-0x87*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe83, 0x0000, 0x0000, /*0x88-0x8f*/
};
static const unsigned short gb18030ext_page49[120] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe85, /*0x40-0x47*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x48-0x4f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x50-0x57*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x58-0x5f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x60-0x67*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x68-0x6f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x70-0x77*/
  0x0000, 0x0000, 0xfe86, 0x0000, 0x0000, 0xfe87, 0x0000, 0x0000, /*0x78-0x7f*/
  0x0000, 0x0000, 0xfe88, 0xfe89, 0x0000, 0xfe8a, 0xfe8b, 0x0000, /*0x80-0x87*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x88-0x8f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x90-0x97*/
  0x0000, 0x0000, 0x0000, 0xfe8d, 0x0000, 0x0000, 0x0000, 0xfe8c, /*0x98-0x9f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xa0-0xa7*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0xa8-0xaf*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe8f, 0xfe8e, /*0xb0-0xb7*/
};
static const unsigned short gb18030ext_page4c[56] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe96, /*0x70-0x77*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x78-0x7f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x80-0x87*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x88-0x8f*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x90-0x97*/
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xfe93, /*0x98-0x9f*/
  0xfe94, 0xfe95, 0xfe97, 0xfe92, 0x0000, 0x0000, 0x0000, 0x0000, /*0xa0-0xa7*/
};
static const unsigned short gb18030ext_page4d[16] = {
  0x0000, 0x0000, 0x0000, 0xfe98, 0xfe99, 0xfe9a, 0xfe9b, 0xfe9c, /*0x10-0x17*/
  0xfe9d, 0xfe9e, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, /*0x18-0x1f*/
};

static int
gb18030ext_wctomb (conv_t conv, unsigned char *r, ucs4_t wc, int n)
{
  if (n >= 2) {
    unsigned short c = 0;
    if (wc == 0x01f9)
      c = 0xa8bf;
    else if (wc == 0x20ac)
      c = 0xa2e3;
    else if (wc >= 0x2e80 && wc < 0x2ed0)
      c = gb18030ext_page2e[wc-0x2e80];
    else if (wc >= 0x2ff0 && wc < 0x3000)
      c = gb18030ext_page2f[wc-0x2ff0];
    else if (wc == 0x303e)
      c = 0xa989;
    else if (wc >= 0x3440 && wc < 0x3478)
      c = gb18030ext_page34[wc-0x3440];
    else if (wc == 0x359e)
      c = 0xfe5a;
    else if (wc >= 0x3608 && wc < 0x3620)
      c = gb18030ext_page36[wc-0x3608];
    else if (wc == 0x3918)
      c = 0xfe60;
    else if (wc == 0x396e)
      c = 0xfe5f;
    else if (wc >= 0x39c8 && wc < 0x39e0)
      c = gb18030ext_page39[wc-0x39c8];
    else if (wc == 0x3a73)
      c = 0xfe64;
    else if (wc == 0x3b4e)
      c = 0xfe68;
    else if (wc == 0x3c6e)
      c = 0xfe69;
    else if (wc == 0x3ce0)
      c = 0xfe6a;
    else if (wc == 0x4056)
      c = 0xfe6f;
    else if (wc == 0x415f)
      c = 0xfe70;
    else if (wc == 0x4337)
      c = 0xfe72;
    else if (wc >= 0x43a8 && wc < 0x43e0)
      c = gb18030ext_page43[wc-0x43a8];
    else if (wc == 0x44d6)
      c = 0xfe7b;
    else if (wc >= 0x4648 && wc < 0x4668)
      c = gb18030ext_page46[wc-0x4648];
    else if (wc >= 0x4720 && wc < 0x4730)
      c = gb18030ext_page47_1[wc-0x4720];
    else if (wc >= 0x4778 && wc < 0x4790)
      c = gb18030ext_page47_2[wc-0x4778];
    else if (wc >= 0x4940 && wc < 0x49b8)
      c = gb18030ext_page49[wc-0x4940];
    else if (wc >= 0x4c70 && wc < 0x4ca8)
      c = gb18030ext_page4c[wc-0x4c70];
    else if (wc >= 0x4d10 && wc < 0x4d20)
      c = gb18030ext_page4d[wc-0x4d10];
    else if (wc == 0x4dae)
      c = 0xfe9f;
    if (c != 0) {
      r[0] = (c >> 8); r[1] = (c & 0xff);
      return 2;
    }
    return RET_ILUNI;
  }
  return RET_TOOSMALL;
}
