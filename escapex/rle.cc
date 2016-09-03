#include "rle.h"

#include <string>
#include "util.h"
#include "extent.h"
#include "escapex.h"
#include <assert.h>

namespace {
// Maybe promote this to its own library in cc-lib?
/* treats strings as buffers of bits */
struct BitBuffer {
  /* read n bits from the string s from bit offset idx.
     (high bits first)
     write that to output and return true.
     if an error occurs (such as going beyond the end of the string),
     then return false, perhaps destroying idx and output */
  template<class UINT>
  static bool nbits(const string &s, int n,
		    unsigned int &idx, UINT &output);

  /* create a new empty bit buffer */
  BitBuffer() : data(0), size(0), bits(0) { }

  /* appends bits to the bit buffer */
  template<class UINT>
  void writebits(int width, UINT thebits);

  /* get the contents of the buffer as a string, 
     padded at the end if necessary */
  string getstring();

  /* give the number of bytes needed to store n bits */
  static int ceil(int bits);


  ~BitBuffer() { free(data); }

  private:
  unsigned char *data;
  int size;
  int bits;
};

int BitBuffer::ceil(int bits) {
  return (bits >> 3) + !!(bits & 7);
}

template<class UINT>
bool BitBuffer::nbits(const string &s, int n, unsigned int &idx, UINT &out) {
# define NTHBIT(x) !! (s[(x) >> 3] & (((UINT)1) << (7 - ((x) & 7))))

  out = 0;

  while (n--) {
    out <<= 1;
    /* check bounds */
    if ((idx >> 3) >= s.length()) return false;
    out |= NTHBIT(idx);
    idx++;
  }
  return true;

# undef NTHBIT
}

string BitBuffer::getstring() {
  int n = ceil(bits);
  if (data) return string((char *)data, n);
  else return "";
}

template<class UINT>
void BitBuffer::writebits(int n, UINT b) {
  /* assumes position already holds 0 */
# define WRITEBIT(x, v) data[(x) >> 3] |= ((!!v) << (7 - ((x) & 7)))

  /* printf("writebits(%d, %d)\n", n, b); */

  for (int i = 0; i < n; i++) {
    int bytes_needed = ceil(bits + 1);

    /* allocate more */
    if (bytes_needed > size) {
      int nsize = (size + 1) * 2;
      uint8 *tmp =
	(uint8 *) malloc(nsize * sizeof (unsigned char));
      if (!tmp) abort();
      memset(tmp, 0, nsize);
      memcpy(tmp, data, size);
      free(data);
      data = tmp;
      size = nsize;
    }

    int bit = !!(b & (((UINT)1) << (n - (i + 1))));
    /* printf("  write %d at %d\n", bit, bits); */
    WRITEBIT(bits, bit);
    bits++;
  }

# undef WRITEBIT
}

}  // namespace


int *EscapeRLE::Decode(const string &s, unsigned int &idx_bytes, int n) {
  int *out = (int*)malloc(n * sizeof (int));
  unsigned int idx = idx_bytes * 8;

  if (!out) return nullptr;
  Extentf<int> eo(out);

  /* number of bytes used to represent one integer. */
  unsigned int bytecount;
  if (!BitBuffer::nbits(s, 8, idx, bytecount)) return nullptr;
  int bits;

  unsigned int framebits = 8;
  if (bytecount & 128) {
    if (bytecount & 64) {
      if (!BitBuffer::nbits(s, 5, idx, framebits)) return nullptr;
      // Framebits of 0 leads to only invalid frames (anti-runs of
      // length 0), and so is illegal.
      if (framebits <= 0 || framebits > 32) return nullptr;
    }
    bits = bytecount & 63;
  } else {
    if (bytecount > 4) {
      printf("Bad file bytecount %d\n", bytecount);
      return nullptr;
    }
    bits = bytecount * 8;
  }

  /* printf("bit count: %d\n", bits); */

  unsigned int run;

  /* out index */
  int oi = 0;

  while (oi < n) {
    if (!BitBuffer::nbits(s, framebits, idx, run)) return nullptr;

    /* printf("[%d] run: %d\n", idx, run); */
    if (run == 0) {
      /* anti-run */
      if (!BitBuffer::nbits(s, framebits, idx, run)) return nullptr;

      /* printf("  .. [%d] anti %d\n", idx, run); */
      if (run == 0) return nullptr; /* illegal */
      for (unsigned int m = 0; m < run; m++) {
	unsigned int ch;
	if (!BitBuffer::nbits(s, bits, idx, ch)) return nullptr;
	if (oi >= n) return nullptr;
	out[oi++] = ch;
      }
    } else {
      unsigned int ch;
      if (!BitBuffer::nbits(s, bits, idx, ch)) return nullptr;

      for (unsigned int m = 0; m < run; m++) {
	if (oi >= n) return nullptr;
	out[oi++] = ch;
      }
    }
  }
  eo.release();
  idx_bytes = BitBuffer::ceil(idx);
  return out;
}

/* 
   PERF this can be more efficient by using a reduced
   number of bits to write frame headers. There is
   already support for this in Decode.
*/
string EscapeRLE::Encode(int n, const int *a) {
  int max = 0;
  for (int j = 0; j < n; j++) {
    if (a[j] > max) max = a[j];
  }

  /* how many bytes to write a single item?
     (see the discussion at EscapeRLE::Encode)

     We avoid using "real" compression schemes
     (such as Huffman encoding) because the
     overhead of dictionaries can often dwarf
     the size of what we're encoding (ie,
     200 move solutions). */

  unsigned int bits = 0;

  {
    unsigned int shift = max;
    for (int i = 0; i <= 32; i++) {
      if (shift & 1) bits = i + 1;
      shift >>= 1;
    }
  }

  /* printf("bits needed: %d\n", bits); */

  BitBuffer ou;

  /* new format has high bit set (see RLE::Decode) */
  ou.writebits(8, bits | 128);

  enum Mode {
    /* back == front */
    NOTHING,
    /* back points to beginning of run */
    RUN,
    /* back points to beginning of antirun */
    ANTIRUN,
    /* back points to char, front to next... */
    CHAR,
    /* done, exit on next loop */
    EXIT,
  };

  Mode mode = NOTHING;
  int back = 0, front = 0;

  while (mode != EXIT) {
    switch (mode) {
    case EXIT:
      assert(false); // Impossible.
    case NOTHING:
      assert(back == front);

      if (front >= n) mode = EXIT; /* done, no backlog */
      else {
	mode = CHAR;
	front++;
      }
      break;
    case CHAR:
      assert(back == (front - 1));

      if (front >= n) {
	/* write a single character */
	ou.writebits(8, 1);
	ou.writebits(bits, a[back]);
	mode = EXIT;
      } else {
	if (a[front] == a[back]) {
	  /* start run */
	  mode = RUN;
	  front++;
	} else {
	  /* start antirun */
	  mode = ANTIRUN;
	  front++;
	}
      }
      break;
    case RUN:
      assert((front - back) >= 2);
      /* from back to front should be same char */

      if (front >= n || a[front] != a[back]) {
	/* write run. */
	while ((front - back) > 0) {
	  unsigned int x = front - back;
	  if (x > 255) x = 255;

	  ou.writebits(8, x);
	  ou.writebits(bits, a[back]);

	  back += x;
	}
	if (front >= n) mode = EXIT;
	else mode = NOTHING;
      } else front++;

      break;
    case ANTIRUN:
      assert((front - back) >= 2);

      if (front >= n ||
	  ((front - back) >= 3 &&
	   (a[front] ==
	    a[front - 1]) &&
	   (a[front] ==
	    a[front - 2]))) {

	if (front >= n) {
	  /* will write tail anti-run below */
	  mode = EXIT;
	} else {
	  /* must be here because we saw a run of 3.
	     we don't want to include this
	     run in the anti-run */
	  front -= 2;
	  /* after writing anti-run, we will
	     be with back = front and in
	     NOTHING state, but we will
	     detect a run. */
	  mode = NOTHING;
	}

	/* write anti-run, unless
	   there's just one character */
	while ((front - back) > 0) {
	  unsigned int x = front - back;
	  if (x > 255) x = 255;

	  if (x == 1) {
	    ou.writebits(8, 1);
	    ou.writebits(bits, a[back]);

	    back++;
	  } else {
	    ou.writebits(8, 0);
	    ou.writebits(8, x);

	    while (x--) {
	      ou.writebits(bits, a[back]);
	      back++;
	    }
	  }
	}
	break;
      } else front++;
    }
  }

  return ou.getstring();
}
