/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */
#include <arm_neon.h>

#include "./av1_rtcd.h"

#include "av1/common/cfl.h"

static INLINE void vldsubstq_s16(int16_t *buf, int16x8_t sub) {
  vst1q_s16(buf, vsubq_s16(vld1q_s16(buf), sub));
}

static INLINE uint16x8_t vldaddq_u16(const uint16_t *buf, size_t offset) {
  return vaddq_u16(vld1q_u16(buf), vld1q_u16(buf + offset));
}

static INLINE void subtract_average_neon(int16_t *pred_buf, int width,
                                         int height, int round_offset,
                                         const int num_pel_log2) {
  const int16_t *const end = pred_buf + height * CFL_BUF_LINE;
  const uint16_t *const sum_end = (uint16_t *)end;

  // Round offset is not needed, because NEON will handle the rounding.
  (void)round_offset;

  // To optimize the use of the CPU pipeline, we process 4 rows per iteration
  const int step = 4 * CFL_BUF_LINE;

  // At this stage, the prediction buffer contains scaled reconstructed luma
  // pixels, which are positive integer and only require 15 bits. By using
  // unsigned integer for the sum, we can do one addition operation inside 16
  // bits (8 lanes) before having to convert to 32 bits (4 lanes).
  const uint16_t *sum_buf = (uint16_t *)pred_buf;
  uint32x4_t sum_32x4 = { 0, 0, 0, 0 };
  do {
    // For all widths, we load, add and combine the data so it fits in 4 lanes.
    if (width == 4) {
      const uint16x4_t a0 =
          vadd_u16(vld1_u16(sum_buf), vld1_u16(sum_buf + CFL_BUF_LINE));
      const uint16x4_t a1 = vadd_u16(vld1_u16(sum_buf + 2 * CFL_BUF_LINE),
                                     vld1_u16(sum_buf + 3 * CFL_BUF_LINE));
      sum_32x4 = vaddq_u32(sum_32x4, vaddl_u16(a0, a1));
    } else if (width == 8) {
      const uint16x8_t a0 = vldaddq_u16(sum_buf, CFL_BUF_LINE);
      const uint16x8_t a1 =
          vldaddq_u16(sum_buf + 2 * CFL_BUF_LINE, CFL_BUF_LINE);
      sum_32x4 = vpadalq_u16(sum_32x4, a0);
      sum_32x4 = vpadalq_u16(sum_32x4, a1);
    } else {
      const uint16x8_t row0 = vldaddq_u16(sum_buf, 8);
      const uint16x8_t row1 = vldaddq_u16(sum_buf + CFL_BUF_LINE, 8);
      const uint16x8_t row2 = vldaddq_u16(sum_buf + 2 * CFL_BUF_LINE, 8);
      const uint16x8_t row3 = vldaddq_u16(sum_buf + 3 * CFL_BUF_LINE, 8);
      sum_32x4 = vpadalq_u16(sum_32x4, row0);
      sum_32x4 = vpadalq_u16(sum_32x4, row1);
      sum_32x4 = vpadalq_u16(sum_32x4, row2);
      sum_32x4 = vpadalq_u16(sum_32x4, row3);

      if (width == 32) {
        const uint16x8_t row0_1 = vldaddq_u16(sum_buf + 16, 8);
        const uint16x8_t row1_1 = vldaddq_u16(sum_buf + CFL_BUF_LINE + 16, 8);
        const uint16x8_t row2_1 =
            vldaddq_u16(sum_buf + 2 * CFL_BUF_LINE + 16, 8);
        const uint16x8_t row3_1 =
            vldaddq_u16(sum_buf + 3 * CFL_BUF_LINE + 16, 8);

        sum_32x4 = vpadalq_u16(sum_32x4, row0_1);
        sum_32x4 = vpadalq_u16(sum_32x4, row1_1);
        sum_32x4 = vpadalq_u16(sum_32x4, row2_1);
        sum_32x4 = vpadalq_u16(sum_32x4, row3_1);
      }
    }
  } while ((sum_buf += step) < sum_end);

  // Permute and add in such a way that each lane contains the block sum.
  // [A+C+B+D, B+D+A+C, C+A+D+B, D+B+C+A]
#if __ARM_ARCH >= 8
  sum_32x4 = vpaddq_u32(sum_32x4, sum_32x4);
  sum_32x4 = vpaddq_u32(sum_32x4, sum_32x4);
#else
  uint32x4_t flip =
      vcombine_u32(vget_high_u32(sum_32x4), vget_low_u32(sum_32x4));
  sum_32x4 = vaddq_u32(sum_32x4, flip);
  sum_32x4 = vaddq_u32(sum_32x4, vrev64q_u32(sum_32x4));
#endif

  // Computing the average could be done using scalars, but getting off the NEON
  // engine introduces latency, so we use vqrshrn.
  int16x4_t avg_16x4;
  // Constant propagation makes for some ugly code.
  switch (num_pel_log2) {
    case 4: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 4)); break;
    case 5: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 5)); break;
    case 6: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 6)); break;
    case 7: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 7)); break;
    case 8: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 8)); break;
    case 9: avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 9)); break;
    case 10:
      avg_16x4 = vreinterpret_s16_u16(vqrshrn_n_u32(sum_32x4, 10));
      break;
    default: assert(0);
  }

  if (width == 4) {
    do {
      vst1_s16(pred_buf, vsub_s16(vld1_s16(pred_buf), avg_16x4));
    } while ((pred_buf += CFL_BUF_LINE) < end);
  } else {
    const int16x8_t avg_16x8 = vcombine_s16(avg_16x4, avg_16x4);
    do {
      vldsubstq_s16(pred_buf, avg_16x8);
      vldsubstq_s16(pred_buf + CFL_BUF_LINE, avg_16x8);
      vldsubstq_s16(pred_buf + 2 * CFL_BUF_LINE, avg_16x8);
      vldsubstq_s16(pred_buf + 3 * CFL_BUF_LINE, avg_16x8);

      if (width > 8) {
        vldsubstq_s16(pred_buf + 8, avg_16x8);
        vldsubstq_s16(pred_buf + CFL_BUF_LINE + 8, avg_16x8);
        vldsubstq_s16(pred_buf + 2 * CFL_BUF_LINE + 8, avg_16x8);
        vldsubstq_s16(pred_buf + 3 * CFL_BUF_LINE + 8, avg_16x8);
      }
      if (width == 32) {
        vldsubstq_s16(pred_buf + 16, avg_16x8);
        vldsubstq_s16(pred_buf + 24, avg_16x8);
        vldsubstq_s16(pred_buf + CFL_BUF_LINE + 16, avg_16x8);
        vldsubstq_s16(pred_buf + CFL_BUF_LINE + 24, avg_16x8);
        vldsubstq_s16(pred_buf + 2 * CFL_BUF_LINE + 16, avg_16x8);
        vldsubstq_s16(pred_buf + 2 * CFL_BUF_LINE + 24, avg_16x8);
        vldsubstq_s16(pred_buf + 3 * CFL_BUF_LINE + 16, avg_16x8);
        vldsubstq_s16(pred_buf + 3 * CFL_BUF_LINE + 24, avg_16x8);
      }
    } while ((pred_buf += step) < end);
  }
}

CFL_SUB_AVG_FN(neon)