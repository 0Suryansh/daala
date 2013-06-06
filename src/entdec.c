/*Daala video codec
Copyright (c) 2001-2013 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#include "entdec.h"

/*A range decoder.
  This is an entropy decoder based upon \cite{Mar79}, which is itself a
   rediscovery of the FIFO arithmetic code introduced by \cite{Pas76}.
  It is very similar to arithmetic encoding, except that encoding is done with
   digits in any base, instead of with bits, and so it is faster when using
   larger bases (i.e.: a byte).
  The author claims an average waste of $\frac{1}{2}\log_b(2b)$ bits, where $b$
   is the base, longer than the theoretical optimum, but to my knowledge there
   is no published justification for this claim.
  This only seems true when using near-infinite precision arithmetic so that
   the process is carried out with no rounding errors.

  An excellent description of implementation details is available at
   http://www.arturocampos.com/ac_range.html
  A recent work \cite{MNW98} which proposes several changes to arithmetic
   encoding for efficiency actually re-discovers many of the principles
   behind range encoding, and presents a good theoretical analysis of them.

  End of stream is handled by writing out the smallest number of bits that
   ensures that the stream will be correctly decoded regardless of the value of
   any subsequent bits.
  od_ec_dec_tell() can be used to determine how many bits were needed to decode
   all the symbols thus far; other data can be packed in the remaining bits of
   the input buffer.
  @PHDTHESIS{Pas76,
    author="Richard Clark Pasco",
    title="Source coding algorithms for fast data compression",
    school="Dept. of Electrical Engineering, Stanford University",
    address="Stanford, CA",
    month=May,
    year=1976
  }
  @INPROCEEDINGS{Mar79,
   author="Martin, G.N.N.",
   title="Range encoding: an algorithm for removing redundancy from a digitised
    message",
   booktitle="Video & Data Recording Conference",
   year=1979,
   address="Southampton",
   month=Jul
  }
  @ARTICLE{MNW98,
   author="Alistair Moffat and Radford Neal and Ian H. Witten",
   title="Arithmetic Coding Revisited",
   journal="{ACM} Transactions on Information Systems",
   year=1998,
   volume=16,
   number=3,
   pages="256--294",
   month=Jul,
   URL="http://www.stanford.edu/class/ee398/handouts/papers/Moffat98ArithmCoding.pdf"
  }*/

/*This is meant to be a large, positive constant that can still be efficiently
   loaded as an immediate (on platforms like ARM, for example).
  Even relatively modest values like 100 would work fine.*/
# define OD_EC_LOTS_OF_BITS (0x4000)

/*Takes updated dif and range values, renormalizes them so that
   32768 <= rng < 65536 (reading more bytes from the stream into dif if
   necessary), and stores them back in the decoder context.
  dif: The new value of dif.
  rng: The new value of the range.
  ret: The value to return.
  Return: ret.
          This allows the compiler to jump to this function via a tail-call.*/
static int od_ec_dec_normalize(od_ec_dec *dec,
 od_ec_window dif, unsigned rng, int ret) {
  int d;
  int c;
  int s;
  c = dec->cnt;
  OD_ASSERT(rng <= 65535U);
  d = 16 - OD_ILOG_NZ(rng);
  c -= d;
  dif <<= d;
  if (c < 0) {
    const unsigned char *end;
    const unsigned char *bptr;
    end = dec->end;
    bptr = dec->bptr;
    for (s = OD_EC_WINDOW_SIZE - 9 -(c + 15); s >= 0;) {
      OD_ASSERT(s <= OD_EC_WINDOW_SIZE - 8);
      if (bptr >= end) {
        dec->tell_offs += OD_EC_LOTS_OF_BITS - c;
        c = OD_EC_LOTS_OF_BITS;
        break;
      }
      dif |= (od_ec_window)*bptr++ << s;
      c += 8;
      s -= 8;
    }
    dec->bptr = bptr;
  }
  dec->dif = dif;
  dec->rng = rng << d;
  dec->cnt = c;
  return ret;
}

/*Initializes the decoder.
  buf: The input buffer to use.
  Return: 0 on success, or a negative value on error.*/
void od_ec_dec_init(od_ec_dec *dec,
 const unsigned char *buf, ogg_uint32_t storage) {
  od_ec_window dif;
  ogg_uint32_t offs;
  ogg_int32_t tell_offs;
  int c;
  int s;
  tell_offs = 10 - (OD_EC_WINDOW_SIZE - 8);
  offs = 0;
  dif = 0;
  c = -15;
  for (s = OD_EC_WINDOW_SIZE - 9; s >= 0;) {
    if (offs >= storage) {
      tell_offs += OD_EC_LOTS_OF_BITS - c;
      c = OD_EC_LOTS_OF_BITS;
      break;
    }
    c += 8;
    dif |= (od_ec_window)buf[offs++] << s;
    s -= 8;
  }
  dec->buf = buf;
  dec->eptr = buf + storage;
  dec->end_window = 0;
  dec->nend_bits = 0;
  dec->tell_offs = tell_offs;
  dec->end = buf + storage;
  dec->bptr = buf + offs;
  dec->dif = dif;
  dec->rng = 0x8000;
  dec->cnt = c;
  dec->error = 0;
}

/*Decode a bit that has an fz/ft probability of being a zero.
  fz: The probability that the bit is zero, scaled by _ft.
  ft: The total probability.
      This must be at least 16384 and no more than 32768.
  Return: The value decoded (0 or 1).*/
int od_ec_decode_bool(od_ec_dec *dec, unsigned fz, unsigned ft) {
  od_ec_window dif;
  od_ec_window vw;
  unsigned r;
  int s;
  unsigned v;
  int ret;
  OD_ASSERT(0 < fz);
  OD_ASSERT(fz < ft);
  OD_ASSERT(16384 <= ft);
  OD_ASSERT(ft <= 32768U);
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(ft <= r);
  s = r - ft >= ft;
  ft <<= s;
  fz <<= s;
  OD_ASSERT(r - ft < ft);
  v = fz + OD_MINI(fz, r - ft);
  vw = (od_ec_window)v << (OD_EC_WINDOW_SIZE - 16);
  ret = dif >= vw;
  if (ret) dif -= vw;
  r = ret ? r - v : v;
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Equivalent to od_ec_decode_bool() with ft == 32768.
  fz: The probability that the bit is zero, scaled by 32768.
  Return: The value decoded (0 or 1).*/
int od_ec_decode_bool_q15(od_ec_dec *dec, unsigned fz) {
  od_ec_window dif;
  od_ec_window vw;
  unsigned r;
  unsigned v;
  int ret;
  OD_ASSERT( 0 < fz);
  OD_ASSERT(fz < 32768U);
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(32768U <= r);
  v = fz + OD_MINI(fz, r - 32768U);
  vw = (od_ec_window)v << (OD_EC_WINDOW_SIZE - 16);
  ret = dif >= vw;
  if (ret) dif -= vw;
  r = ret ? r - v : v;
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Decodes a symbol given a cumulative distribution function (CDF) table.
  cdf: The CDF, such that symbol s falls in the range
        [s > 0 ? cdf[s - 1] : 0, cdf[s]).
       The values must be monotonically non-increasing, and cdf[nsyms - 1]
        must be at least 16384, and no more than 32768.
  nsyms: The number of symbols in the alphabet.
         This should be at most 16.
  Return: The decoded symbol s.*/
int od_ec_decode_cdf(od_ec_dec *dec, const ogg_uint16_t *cdf, int nsyms) {
  od_ec_window dif;
  unsigned r;
  unsigned d;
  int s;
  unsigned u;
  unsigned v;
  unsigned q;
  unsigned fl;
  unsigned fh;
  unsigned ft;
  int ret;
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(nsyms > 0);
  ft = cdf[nsyms - 1];
  OD_ASSERT(16384 <= ft);
  OD_ASSERT(ft <= 32768U);
  OD_ASSERT(ft <= r);
  s = r - ft >= ft;
  ft <<= s;
  d = r - ft;
  OD_ASSERT(d < ft);
  q = OD_MAXI((int)(dif >> (OD_EC_WINDOW_SIZE - 15)),
   (int)((dif >> (OD_EC_WINDOW_SIZE - 16)) - d)) >> s;
  OD_ASSERT(q < ft >> s);
  fl = 0;
  ret = 0;
  for (fh = cdf[ret]; fh <= q; fh = cdf[++ret]) fl = fh;
  OD_ASSERT(fh <= ft >> s);
  fl <<= s;
  fh <<= s;
  u = fl + OD_MINI(fl, d);
  v = fh + OD_MINI(fh, d);
  r = v - u;
  dif -= (od_ec_window)u << (OD_EC_WINDOW_SIZE - 16);
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Decodes a symbol given a cumulative distribution function (CDF) table.
  cdf: The CDF, such that symbol s falls in the range
        [s > 0 ? cdf[s - 1] : 0, cdf[s]).
       The values must be monotonically non-increasing, and cdf[nsyms - 1]
        must be 32768.
  nsyms: The number of symbols in the alphabet.
         This should be at most 16.
  Return: The decoded symbol s.*/
int od_ec_decode_cdf_q15(od_ec_dec *dec, const ogg_uint16_t *cdf, int nsyms) {
  od_ec_window dif;
  unsigned r;
  unsigned d;
  unsigned u;
  unsigned v;
  unsigned q;
  unsigned fl;
  unsigned fh;
  int ret;
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(nsyms > 0);
  OD_ASSERT(cdf[nsyms - 1] == 32768U);
  OD_ASSERT(32768U <= r);
  d = r - 32768U;
  OD_ASSERT(d < 32768U);
  q = OD_MAXI((int)(dif >> (OD_EC_WINDOW_SIZE -15)),
   (int)((dif >> (OD_EC_WINDOW_SIZE - 16)) - d));
  OD_ASSERT(q < 32768U);
  fl = 0;
  ret = 0;
  for (fh = cdf[ret]; fh <= q; fh = cdf[++ret]) fl = fh;
  OD_ASSERT(fh <= 32768U);
  u = fl + OD_MINI(fl, d);
  v = fh + OD_MINI(fh, d);
  r = v - u;
  dif -= (od_ec_window)u << (OD_EC_WINDOW_SIZE - 16);
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Decodes a symbol given a cumulative distribution function (CDF) table.
  cdf: The CDF, such that symbol s falls in the range
        [s > 0 ? cdf[s - 1] : 0, cdf[s]).
       The values must be monotonically non-increasing, and cdf[nsyms - 1]
       must be at least 2, and no more than 32768.
  nsyms: The number of symbols in the alphabet.
         This should be at most 16.
  Return: The decoded symbol s.*/
int od_ec_decode_cdf_unscaled(od_ec_dec *dec,
 const ogg_uint16_t *cdf, int nsyms) {
  od_ec_window dif;
  unsigned r;
  unsigned d;
  int s;
  unsigned u;
  unsigned v;
  unsigned q;
  unsigned fl;
  unsigned fh;
  unsigned ft;
  int ret;
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(nsyms > 0);
  ft = cdf[nsyms - 1];
  OD_ASSERT(2 <= ft);
  OD_ASSERT(ft <= 32768U);
  s = 15 - OD_ILOG_NZ(ft - 1);
  ft <<= s;
  OD_ASSERT(ft <= r);
  if (r - ft >= ft) {
    ft <<= 1;
    s++;
  }
  d = r - ft;
  OD_ASSERT(d < ft);
  q = OD_MAXI((int)(dif >> (OD_EC_WINDOW_SIZE - 15)),
   (int)((dif >> (OD_EC_WINDOW_SIZE - 16)) - d)) >> s;
  OD_ASSERT(q < ft >> s);
  fl = 0;
  ret = 0;
  for (fh = cdf[ret]; fh <= q; fh = cdf[++ret]) fl = fh;
  OD_ASSERT(fh <= ft >> s);
  fl <<= s;
  fh <<= s;
  u = fl + OD_MINI(fl, d);
  v = fh + OD_MINI(fh, d);
  r = v - u;
  dif -= (od_ec_window)u << (OD_EC_WINDOW_SIZE - 16);
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Decodes a symbol given a cumulative distribution function (CDF) table.
  cdf: The CDF, such that symbol s falls in the range
        [s > 0 ? cdf[s - 1] : 0, cdf[s]).
       The values must be monotonically non-increasing, and cdf[nsyms - 1]
       must be exactly 1 << ftb.
  nsyms: The number of symbols in the alphabet.
         This should be at most 16.
  ftb: The number of bits of precision in the cumulative distribution.
       This must be no more than 15.
  Return: The decoded symbol s.*/
int od_ec_decode_cdf_unscaled_dyadic(od_ec_dec *dec,
 const ogg_uint16_t *cdf, int nsyms, unsigned ftb) {
  od_ec_window dif;
  unsigned r;
  unsigned d;
  int s;
  unsigned u;
  unsigned v;
  unsigned q;
  unsigned fl;
  unsigned fh;
  int ret;
  dif = dec->dif;
  r = dec->rng;
  OD_ASSERT(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  OD_ASSERT(ftb <= 15);
  OD_ASSERT(cdf[nsyms - 1] == 1U << ftb);
  s = 15 - ftb;
  OD_ASSERT(32768U <= r);
  d = r - 32768U;
  OD_ASSERT(d < 32768U);
  q = OD_MAXI((int)(dif >> (OD_EC_WINDOW_SIZE - 15)),
   (int)((dif >> (OD_EC_WINDOW_SIZE - 16)) - d)) >> s;
  OD_ASSERT(q < 1U << ftb);
  fl = 0;
  ret = 0;
  for (fh = cdf[ret]; fh <= q; fh = cdf[++ret]) fl = fh;
  OD_ASSERT(fh <= 1U << ftb);
  fl <<= s;
  fh <<= s;
  u = fl + OD_MINI(fl, d);
  v = fh + OD_MINI(fh, d);
  r = v - u;
  dif -= (od_ec_window)u << (OD_EC_WINDOW_SIZE - 16);
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/*Extracts a raw unsigned integer with a non-power-of-2 range from the stream.
  The integer must have been encoded with od_ec_enc_uint().
  ft: The number of integers that can be decoded (one more than the max).
      This must be at least 2, and no more than 2**29.
  Return: The decoded bits.*/
ogg_uint32_t od_ec_dec_uint(od_ec_dec *dec, ogg_uint32_t ft) {
  OD_ASSERT(ft >= 2);
  OD_ASSERT(ft <= (ogg_uint32_t)1 << (25 + OD_EC_UINT_BITS));
  if (ft > 1U << OD_EC_UINT_BITS) {
    ogg_uint32_t t;
    int ft1;
    int ftb;
    ft--;
    ftb = OD_ILOG_NZ(ft) - OD_EC_UINT_BITS;
    ft1 = (int)(ft >> ftb) + 1;
    t = od_ec_decode_cdf_q15(dec, OD_UNIFORM_CDF_Q15(ft1), ft1);
    t = t << ftb | od_ec_dec_bits(dec, ftb);
    if (t <= ft) return t;
    dec->error = 1;
    return ft;
  }
  return od_ec_decode_cdf_q15(dec, OD_UNIFORM_CDF_Q15(ft), (int)ft);
}

/*Extracts a sequence of raw bits from the stream.
  The bits must have been encoded with od_ec_enc_bits().
  ftb: The number of bits to extract.
       This must be between 0 and 25, inclusive.
  Return: The decoded bits.*/
ogg_uint32_t od_ec_dec_bits(od_ec_dec *dec, unsigned ftb) {
  od_ec_window window;
  int available;
  ogg_uint32_t ret;
  OD_ASSERT(ftb <= 25);
  window = dec->end_window;
  available = dec->nend_bits;
  if ((unsigned)available < ftb) {
    const unsigned char *buf;
    const unsigned char *eptr;
    buf = dec->buf;
    eptr = dec->eptr;
    OD_ASSERT(available <= OD_EC_WINDOW_SIZE - 8);
    do {
      if (eptr <= buf) {
        dec->tell_offs += OD_EC_LOTS_OF_BITS - available;
        available = OD_EC_LOTS_OF_BITS;
        break;
      }
      window |= *--eptr << available;
      available += 8;
    }
    while (available <= OD_EC_WINDOW_SIZE - 8);
    dec->eptr = eptr;
  }
  ret = (ogg_uint32_t)window & (((ogg_uint32_t)1 << ftb) - 1);
  window >>= ftb;
  available -= ftb;
  dec->end_window = window;
  dec->nend_bits = available;
  return ret;
}

/*Returns the number of bits "used" by the decoded symbols so far.
  This same number can be computed in either the encoder or the decoder, and is
   suitable for making coding decisions.
  Return: The number of bits.
          This will always be slightly larger than the exact value (e.g., all
           rounding error is in the positive direction).*/
int od_ec_dec_tell(od_ec_dec *dec) {
  return ((dec->end - dec->eptr) + (dec->bptr - dec->buf))*8
   - dec->cnt - dec->nend_bits + dec->tell_offs;
}

/*Returns the number of bits "used" by the decoded symbols so far.
  This same number can be computed in either the encoder or the decoder, and is
   suitable for making coding decisions.
  Return: The number of bits scaled by 2**OD_BITRES.
          This will always be slightly larger than the exact value (e.g., all
           rounding error is in the positive direction).*/
ogg_uint32_t od_ec_dec_tell_frac(od_ec_dec *dec) {
  return od_ec_tell_frac(od_ec_dec_tell(dec), dec->rng);
}
