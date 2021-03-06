/*
 * Copyright © 2021 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */

/* Make the test not meaningless when asserts are disabled. */
#undef NDEBUG

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <amdgpu.h>
#include "drm-uapi/amdgpu_drm.h"
#include "drm-uapi/drm_fourcc.h"

#include "ac_surface.h"
#include "util/macros.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_vector.h"
#include "util/mesa-sha1.h"
#include "addrlib/inc/addrinterface.h"

#include "ac_surface_test_common.h"

/*
 * The main goal of this test is to validate that our dcc addressing functions
 * match addrlib behavior.
 */

/* DCC address computation without mipmapping. */
static unsigned gfx9_dcc_addr_from_coord(const struct radeon_info *info,
                                         /* Shader key inputs: */
                                         /* equation varies with resource_type, swizzle_mode,
                                          * bpp, number of fragments, pipe_aligned, rb_aligned */
                                         ADDR2_COMPUTE_DCCINFO_OUTPUT *eq,
                                         unsigned meta_block_width, unsigned meta_block_height,
                                         unsigned meta_block_depth,
                                         /* Shader inputs: */
                                         unsigned dcc_pitch, unsigned dcc_height,
                                         unsigned x, unsigned y, unsigned z,
                                         unsigned sample, unsigned pipe_xor)
{
   /* The compiled shader shouldn't be complicated considering there are a lot of constants here. */
   unsigned meta_block_width_log2 = util_logbase2(meta_block_width);
   unsigned meta_block_height_log2 = util_logbase2(meta_block_height);
   unsigned meta_block_depth_log2 = util_logbase2(meta_block_depth);

   unsigned m_pipeInterleaveLog2 = 8 + G_0098F8_PIPE_INTERLEAVE_SIZE_GFX9(info->gb_addr_config);
   unsigned numPipeBits = eq->equation.gfx9.numPipeBits;
   unsigned pitchInBlock = dcc_pitch >> meta_block_width_log2;
   unsigned sliceSizeInBlock = (dcc_height >> meta_block_height_log2) * pitchInBlock;

   unsigned xb = x >> meta_block_width_log2;
   unsigned yb = y >> meta_block_height_log2;
   unsigned zb = z >> meta_block_depth_log2;

   unsigned blockIndex = zb * sliceSizeInBlock + yb * pitchInBlock + xb;
   unsigned coords[] = {x, y, z, sample, blockIndex};

   unsigned address = 0;
   unsigned num_bits = eq->equation.gfx9.num_bits;
   assert(num_bits <= 32);

   /* Compute the address up until the last bit that doesn't use the block index. */
   for (unsigned b = 0; b < num_bits - 1; b++) {
      unsigned xor = 0;
      for (unsigned c = 0; c < 5; c++) {
         if (eq->equation.gfx9.bit[b].coord[c].dim >= 5)
            continue;

         assert(eq->equation.gfx9.bit[b].coord[c].ord < 32);
         unsigned ison = (coords[eq->equation.gfx9.bit[b].coord[c].dim] >>
                                 eq->equation.gfx9.bit[b].coord[c].ord) & 0x1;

         xor ^= ison;
      }
      address |= xor << b;
   }

   /* Fill the remaining bits with the block index. */
   unsigned last = num_bits - 1;
   address |= (blockIndex >> eq->equation.gfx9.bit[last].coord[0].ord) << last;

   unsigned pipeXor = pipe_xor & ((1 << numPipeBits) - 1);
   return (address >> 1) ^ (pipeXor << m_pipeInterleaveLog2);
}

/* DCC address computation without mipmapping and MSAA. */
static unsigned gfx10_dcc_addr_from_coord(const struct radeon_info *info,
                                          /* Shader key inputs: */
                                          /* equation varies with bpp and pipe_aligned */
                                          const uint16_t *equation, unsigned bpp,
                                          unsigned meta_block_width, unsigned meta_block_height,
                                          /* Shader inputs: */
                                          unsigned dcc_pitch, unsigned dcc_slice_size,
                                          unsigned x, unsigned y, unsigned z,
                                          unsigned pipe_xor)
{
   /* The compiled shader shouldn't be complicated considering there are a lot of constants here. */
   unsigned bpp_log2 = util_logbase2(bpp >> 3);
   unsigned meta_block_width_log2 = util_logbase2(meta_block_width);
   unsigned meta_block_height_log2 = util_logbase2(meta_block_height);
   unsigned blkSizeLog2 = meta_block_width_log2 + meta_block_height_log2 + bpp_log2 - 8;

   unsigned coord[] = {x, y, z, 0};
   unsigned address = 0;

   for (unsigned i = 0; i < blkSizeLog2 + 1; i++) {
      unsigned v = 0;

      for (unsigned c = 0; c < 4; c++) {
         if (equation[i*4+c] != 0) {
            unsigned mask = equation[i*4+c];
            unsigned bits = coord[c];

            while (mask)
               v ^= (bits >> u_bit_scan(&mask)) & 0x1;
         }
      }

      address |= v << i;
   }

   unsigned blkMask = (1 << blkSizeLog2) - 1;
   unsigned pipeMask = (1 << G_0098F8_NUM_PIPES(info->gb_addr_config)) - 1;
   unsigned m_pipeInterleaveLog2 = 8 + G_0098F8_PIPE_INTERLEAVE_SIZE_GFX9(info->gb_addr_config);
   unsigned xb = x >> meta_block_width_log2;
   unsigned yb = y >> meta_block_height_log2;
   unsigned pb = dcc_pitch >> meta_block_width_log2;
   unsigned blkIndex = (yb * pb) + xb;
   unsigned pipeXor = ((pipe_xor & pipeMask) << m_pipeInterleaveLog2) & blkMask;

   return (dcc_slice_size * z) +
          (blkIndex * (1 << blkSizeLog2)) +
          ((address >> 1) ^ pipeXor);
}

static bool one_dcc_address_test(const char *name, const char *test, ADDR_HANDLE addrlib,
                                 const struct radeon_info *info, unsigned width, unsigned height,
                                 unsigned depth, unsigned samples, unsigned bpp,
                                 unsigned swizzle_mode, bool pipe_aligned, bool rb_aligned,
                                 unsigned mrt_index,
                                 unsigned start_x, unsigned start_y, unsigned start_z,
                                 unsigned start_sample)
{
   ADDR2_COMPUTE_PIPEBANKXOR_INPUT xin = {sizeof(ADDR2_COMPUTE_PIPEBANKXOR_INPUT)};
   ADDR2_COMPUTE_PIPEBANKXOR_OUTPUT xout = {sizeof(ADDR2_COMPUTE_PIPEBANKXOR_OUTPUT)};
   ADDR2_COMPUTE_DCCINFO_INPUT din = {sizeof(din)};
   ADDR2_COMPUTE_DCCINFO_OUTPUT dout = {sizeof(dout)};
   ADDR2_COMPUTE_DCC_ADDRFROMCOORD_INPUT in = {sizeof(in)};
   ADDR2_COMPUTE_DCC_ADDRFROMCOORD_OUTPUT out = {sizeof(out)};
   ADDR2_META_MIP_INFO meta_mip_info[RADEON_SURF_MAX_LEVELS] = {0};

   dout.pMipInfo = meta_mip_info;

   /* Compute DCC info. */
   in.dccKeyFlags.pipeAligned = din.dccKeyFlags.pipeAligned = pipe_aligned;
   in.dccKeyFlags.rbAligned = din.dccKeyFlags.rbAligned = rb_aligned;
   xin.resourceType = in.resourceType = din.resourceType = ADDR_RSRC_TEX_2D;
   xin.swizzleMode = in.swizzleMode = din.swizzleMode = swizzle_mode;
   in.bpp = din.bpp = bpp;
   xin.numFrags = xin.numSamples = in.numFrags = din.numFrags = samples;
   in.numMipLevels = din.numMipLevels = 1; /* addrlib can't do DccAddrFromCoord with mipmapping */
   din.unalignedWidth = width;
   din.unalignedHeight = height;
   din.numSlices = depth;
   din.firstMipIdInTail = 1;

   int ret = Addr2ComputeDccInfo(addrlib, &din, &dout);
   assert(ret == ADDR_OK);

   /* Compute xor. */
   static AddrFormat format[] = {
      ADDR_FMT_8,
      ADDR_FMT_16,
      ADDR_FMT_32,
      ADDR_FMT_32_32,
      ADDR_FMT_32_32_32_32,
   };
   xin.flags.color = 1;
   xin.flags.texture = 1;
   xin.flags.opt4space = 1;
   xin.flags.metaRbUnaligned = !rb_aligned;
   xin.flags.metaPipeUnaligned = !pipe_aligned;
   xin.format = format[util_logbase2(bpp / 8)];
   xin.surfIndex = mrt_index;

   ret = Addr2ComputePipeBankXor(addrlib, &xin, &xout);
   assert(ret == ADDR_OK);

   /* Compute addresses */
   in.compressBlkWidth = dout.compressBlkWidth;
   in.compressBlkHeight = dout.compressBlkHeight;
   in.compressBlkDepth = dout.compressBlkDepth;
   in.metaBlkWidth = dout.metaBlkWidth;
   in.metaBlkHeight = dout.metaBlkHeight;
   in.metaBlkDepth = dout.metaBlkDepth;
   in.dccRamSliceSize = dout.dccRamSliceSize;

   in.mipId = 0;
   in.pitch = dout.pitch;
   in.height = dout.height;
   in.pipeXor = xout.pipeBankXor;

   /* Validate that the packed gfx9_dcc_equation structure can fit all fields. */
   const struct gfx9_dcc_equation eq;
   if (info->chip_class == GFX9) {
      /* The bit array is smaller in gfx9_dcc_equation than in addrlib. */
      assert(dout.equation.gfx9.num_bits <= ARRAY_SIZE(eq.u.gfx9.bit));
   } else {
      /* gfx9_dcc_equation doesn't store the first 4 and the last 8 elements. They must be 0. */
      for (unsigned i = 0; i < 4; i++)
         assert(dout.equation.gfx10_bits[i] == 0);

      for (unsigned i = ARRAY_SIZE(eq.u.gfx10_bits) + 4; i < 68; i++)
         assert(dout.equation.gfx10_bits[i] == 0);
   }

   for (in.x = start_x; in.x < in.pitch; in.x += dout.compressBlkWidth) {
      for (in.y = start_y; in.y < in.height; in.y += dout.compressBlkHeight) {
         for (in.slice = start_z; in.slice < depth; in.slice += dout.compressBlkDepth) {
            for (in.sample = start_sample; in.sample < samples; in.sample++) {
               int r = Addr2ComputeDccAddrFromCoord(addrlib, &in, &out);
               if (r != ADDR_OK) {
                  printf("%s addrlib error: %s\n", name, test);
                  abort();
               }

               unsigned addr;
               if (info->chip_class == GFX9) {
                  addr = gfx9_dcc_addr_from_coord(info, &dout, dout.metaBlkWidth, dout.metaBlkHeight,
                                                  dout.metaBlkDepth, dout.pitch, dout.height,
                                                  in.x, in.y, in.slice, in.sample, in.pipeXor);
                  if (in.sample == 1) {
                     /* Sample 0 should be one byte before sample 1. The DCC MSAA clear relies on it. */
                     assert(addr - 1 ==
                            gfx9_dcc_addr_from_coord(info, &dout, dout.metaBlkWidth, dout.metaBlkHeight,
                                                     dout.metaBlkDepth, dout.pitch, dout.height,
                                                     in.x, in.y, in.slice, 0, in.pipeXor));
                  }
               } else {
                  addr = gfx10_dcc_addr_from_coord(info, dout.equation.gfx10_bits,
                                                   in.bpp, dout.metaBlkWidth, dout.metaBlkHeight,
                                                   dout.pitch, dout.dccRamSliceSize,
                                                   in.x, in.y, in.slice, in.pipeXor);
               }

               if (out.addr != addr) {
                  printf("%s fail (%s) at %ux%ux%u@%u: expected = %llu, got = %u\n",
                         name, test, in.x, in.y, in.slice, in.sample, out.addr, addr);
                  return false;
               }
            }
         }
      }
   }
   return true;
}

static void run_dcc_address_test(const char *name, const struct radeon_info *info, bool full)
{
   unsigned total = 0;
   unsigned fails = 0;
   unsigned swizzle_mode = info->chip_class == GFX9 ? ADDR_SW_64KB_S_X : ADDR_SW_64KB_R_X;
   unsigned last_size, max_samples, min_bpp, max_bpp;

   if (full) {
      last_size = 6*6 - 1;
      max_samples = 8;
      min_bpp = 8;
      max_bpp = 128;
   } else {
      /* The test coverage is reduced for Gitlab CI because it timeouts. */
      last_size = 0;
      max_samples = 2;
      min_bpp = 32;
      max_bpp = 64;
   }

#ifdef HAVE_OPENMP
#pragma omp parallel for
#endif
   for (unsigned size = 0; size <= last_size; size++) {
      unsigned width = 8 + 379 * (size % 6);
      unsigned height = 8 + 379 * ((size / 6) % 6);

      struct ac_addrlib *ac_addrlib = ac_addrlib_create(info, NULL);
      ADDR_HANDLE addrlib = ac_addrlib_get_handle(ac_addrlib);

      unsigned local_fails = 0;
      unsigned local_total = 0;

      for (unsigned bpp = min_bpp; bpp <= max_bpp; bpp *= 2) {
         /* addrlib can do DccAddrFromCoord with MSAA images only on gfx9 */
         for (unsigned samples = 1; samples <= (info->chip_class == GFX9 ? max_samples : 1); samples *= 2) {
            for (int rb_aligned = true; rb_aligned >= (samples > 1 ? true : false); rb_aligned--) {
               for (int pipe_aligned = true; pipe_aligned >= (samples > 1 ? true : false); pipe_aligned--) {
                  for (unsigned mrt_index = 0; mrt_index < 2; mrt_index++) {
                     unsigned depth = 2;
                     char test[256];

                     snprintf(test, sizeof(test), "%ux%ux%u %ubpp %u samples rb:%u pipe:%u",
                              width, height, depth, bpp, samples, rb_aligned, pipe_aligned);

                     if (one_dcc_address_test(name, test, addrlib, info, width, height, depth, samples,
                                              bpp, swizzle_mode, pipe_aligned, rb_aligned, mrt_index,
                                              0, 0, 0, 0)) {
                     } else {
                        local_fails++;
                     }
                     local_total++;
                  }
               }
            }
         }
      }

      ac_addrlib_destroy(ac_addrlib);
      p_atomic_add(&fails, local_fails);
      p_atomic_add(&total, local_total);
   }
   printf("%16s total: %u, fail: %u\n", name, total, fails);
}

int main(int argc, char **argv)
{
   bool full = false;

   if (argc == 2 && !strcmp(argv[1], "--full"))
      full = true;
   else
      puts("Specify --full to run the full test.");

   for (unsigned i = 0; i < ARRAY_SIZE(testcases); ++i) {
      struct radeon_info info = get_radeon_info(&testcases[i]);

      run_dcc_address_test(testcases[i].name, &info, full);
   }

   return 0;
}
