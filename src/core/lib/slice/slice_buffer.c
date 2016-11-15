/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/slice_buffer.h>
#include <grpc/support/port_platform.h>

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/useful.h>

/* grow a buffer; requires GRPC_SLICE_BUFFER_INLINE_ELEMENTS > 1 */
#define GROW(x) (3 * (x) / 2)

static void maybe_embiggen(grpc_slice_buffer *sb) {
  if (sb->count == sb->capacity) {
    sb->capacity = GROW(sb->capacity);
    GPR_ASSERT(sb->capacity > sb->count);
    if (sb->slices == sb->inlined) {
      sb->slices = gpr_malloc(sb->capacity * sizeof(grpc_slice));
      memcpy(sb->slices, sb->inlined, sb->count * sizeof(grpc_slice));
    } else {
      sb->slices = gpr_realloc(sb->slices, sb->capacity * sizeof(grpc_slice));
    }
  }
}

void grpc_slice_buffer_init(grpc_slice_buffer *sb) {
  sb->count = 0;
  sb->length = 0;
  sb->capacity = GRPC_SLICE_BUFFER_INLINE_ELEMENTS;
  sb->slices = sb->inlined;
}

void grpc_slice_buffer_destroy(grpc_slice_buffer *sb) {
  grpc_slice_buffer_reset_and_unref(sb);
  if (sb->slices != sb->inlined) {
    gpr_free(sb->slices);
  }
}

uint8_t *grpc_slice_buffer_tiny_add(grpc_slice_buffer *sb, size_t n) {
  grpc_slice *back;
  uint8_t *out;

  sb->length += n;

  if (sb->count == 0) goto add_new;
  back = &sb->slices[sb->count - 1];
  if (back->refcount) goto add_new;
  if ((back->data.inlined.length + n) > sizeof(back->data.inlined.bytes))
    goto add_new;
  out = back->data.inlined.bytes + back->data.inlined.length;
  back->data.inlined.length = (uint8_t)(back->data.inlined.length + n);
  return out;

add_new:
  maybe_embiggen(sb);
  back = &sb->slices[sb->count];
  sb->count++;
  back->refcount = NULL;
  back->data.inlined.length = (uint8_t)n;
  return back->data.inlined.bytes;
}

size_t grpc_slice_buffer_add_indexed(grpc_slice_buffer *sb, grpc_slice s) {
  size_t out = sb->count;
  maybe_embiggen(sb);
  sb->slices[out] = s;
  sb->length += GRPC_SLICE_LENGTH(s);
  sb->count = out + 1;
  return out;
}

void grpc_slice_buffer_add(grpc_slice_buffer *sb, grpc_slice s) {
  size_t n = sb->count;
  /* if both the last slice in the slice buffer and the slice being added
     are inlined (that is, that they carry their data inside the slice data
     structure), and the back slice is not full, then concatenate directly
     into the back slice, preventing many small slices being passed into
     writes */
  if (!s.refcount && n) {
    grpc_slice *back = &sb->slices[n - 1];
    if (!back->refcount &&
        back->data.inlined.length < GRPC_SLICE_INLINED_SIZE) {
      if (s.data.inlined.length + back->data.inlined.length <=
          GRPC_SLICE_INLINED_SIZE) {
        memcpy(back->data.inlined.bytes + back->data.inlined.length,
               s.data.inlined.bytes, s.data.inlined.length);
        back->data.inlined.length =
            (uint8_t)(back->data.inlined.length + s.data.inlined.length);
      } else {
        size_t cp1 = GRPC_SLICE_INLINED_SIZE - back->data.inlined.length;
        memcpy(back->data.inlined.bytes + back->data.inlined.length,
               s.data.inlined.bytes, cp1);
        back->data.inlined.length = GRPC_SLICE_INLINED_SIZE;
        maybe_embiggen(sb);
        back = &sb->slices[n];
        sb->count = n + 1;
        back->refcount = NULL;
        back->data.inlined.length = (uint8_t)(s.data.inlined.length - cp1);
        memcpy(back->data.inlined.bytes, s.data.inlined.bytes + cp1,
               s.data.inlined.length - cp1);
      }
      sb->length += s.data.inlined.length;
      return; /* early out */
    }
  }
  grpc_slice_buffer_add_indexed(sb, s);
}

void grpc_slice_buffer_addn(grpc_slice_buffer *sb, grpc_slice *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    grpc_slice_buffer_add(sb, s[i]);
  }
}

void grpc_slice_buffer_pop(grpc_slice_buffer *sb) {
  if (sb->count != 0) {
    size_t count = --sb->count;
    sb->length -= GRPC_SLICE_LENGTH(sb->slices[count]);
  }
}

void grpc_slice_buffer_reset_and_unref(grpc_slice_buffer *sb) {
  size_t i;

  for (i = 0; i < sb->count; i++) {
    grpc_slice_unref(sb->slices[i]);
  }

  sb->count = 0;
  sb->length = 0;
}

void grpc_slice_buffer_swap(grpc_slice_buffer *a, grpc_slice_buffer *b) {
  GPR_SWAP(size_t, a->count, b->count);
  GPR_SWAP(size_t, a->capacity, b->capacity);
  GPR_SWAP(size_t, a->length, b->length);

  if (a->slices == a->inlined) {
    if (b->slices == b->inlined) {
      /* swap contents of inlined buffer */
      grpc_slice temp[GRPC_SLICE_BUFFER_INLINE_ELEMENTS];
      memcpy(temp, a->slices, b->count * sizeof(grpc_slice));
      memcpy(a->slices, b->slices, a->count * sizeof(grpc_slice));
      memcpy(b->slices, temp, b->count * sizeof(grpc_slice));
    } else {
      /* a is inlined, b is not - copy a inlined into b, fix pointers */
      a->slices = b->slices;
      b->slices = b->inlined;
      memcpy(b->slices, a->inlined, b->count * sizeof(grpc_slice));
    }
  } else if (b->slices == b->inlined) {
    /* b is inlined, a is not - copy b inlined int a, fix pointers */
    b->slices = a->slices;
    a->slices = a->inlined;
    memcpy(a->slices, b->inlined, a->count * sizeof(grpc_slice));
  } else {
    /* no inlining: easy swap */
    GPR_SWAP(grpc_slice *, a->slices, b->slices);
  }
}

void grpc_slice_buffer_move_into(grpc_slice_buffer *src,
                                 grpc_slice_buffer *dst) {
  /* anything to move? */
  if (src->count == 0) {
    return;
  }
  /* anything in dst? */
  if (dst->count == 0) {
    grpc_slice_buffer_swap(src, dst);
    return;
  }
  /* both buffers have data - copy, and reset src */
  grpc_slice_buffer_addn(dst, src->slices, src->count);
  src->count = 0;
  src->length = 0;
}

void grpc_slice_buffer_move_first(grpc_slice_buffer *src, size_t n,
                                  grpc_slice_buffer *dst) {
  size_t src_idx;
  size_t output_len = dst->length + n;
  size_t new_input_len = src->length - n;
  GPR_ASSERT(src->length >= n);
  if (src->length == n) {
    grpc_slice_buffer_move_into(src, dst);
    return;
  }
  src_idx = 0;
  while (src_idx < src->capacity) {
    grpc_slice slice = src->slices[src_idx];
    size_t slice_len = GRPC_SLICE_LENGTH(slice);
    if (n > slice_len) {
      grpc_slice_buffer_add(dst, slice);
      n -= slice_len;
      src_idx++;
    } else if (n == slice_len) {
      grpc_slice_buffer_add(dst, slice);
      src_idx++;
      break;
    } else { /* n < slice_len */
      src->slices[src_idx] = grpc_slice_split_tail(&slice, n);
      GPR_ASSERT(GRPC_SLICE_LENGTH(slice) == n);
      GPR_ASSERT(GRPC_SLICE_LENGTH(src->slices[src_idx]) == slice_len - n);
      grpc_slice_buffer_add(dst, slice);
      break;
    }
  }
  GPR_ASSERT(dst->length == output_len);
  memmove(src->slices, src->slices + src_idx,
          sizeof(grpc_slice) * (src->count - src_idx));
  src->count -= src_idx;
  src->length = new_input_len;
  GPR_ASSERT(src->count > 0);
}

void grpc_slice_buffer_trim_end(grpc_slice_buffer *sb, size_t n,
                                grpc_slice_buffer *garbage) {
  GPR_ASSERT(n <= sb->length);
  sb->length -= n;
  for (;;) {
    size_t idx = sb->count - 1;
    grpc_slice slice = sb->slices[idx];
    size_t slice_len = GRPC_SLICE_LENGTH(slice);
    if (slice_len > n) {
      sb->slices[idx] = grpc_slice_split_head(&slice, slice_len - n);
      grpc_slice_buffer_add_indexed(garbage, slice);
      return;
    } else if (slice_len == n) {
      grpc_slice_buffer_add_indexed(garbage, slice);
      sb->count = idx;
      return;
    } else {
      grpc_slice_buffer_add_indexed(garbage, slice);
      n -= slice_len;
      sb->count = idx;
    }
  }
}

grpc_slice grpc_slice_buffer_take_first(grpc_slice_buffer *sb) {
  grpc_slice slice;
  GPR_ASSERT(sb->count > 0);
  slice = sb->slices[0];
  memmove(&sb->slices[0], &sb->slices[1], (sb->count - 1) * sizeof(grpc_slice));
  sb->count--;
  sb->length -= GRPC_SLICE_LENGTH(slice);
  return slice;
}