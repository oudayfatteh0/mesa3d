#include "nouveau_device_info.h"

#include "nvidia/g_nv_name_released.h"
#include "util/macros.h"

#include <assert.h>

const char *
name_for_chip(uint32_t dev_id,
              uint16_t subsystem_id,
              uint16_t subsystem_vendor_id)
{
   const char *name = NULL;
   for (uint32_t i = 0; i < ARRAY_SIZE(sChipsReleased); i++) {
      const CHIPS_RELEASED *chip = &sChipsReleased[i];

      if (dev_id != chip->devID)
         continue;

      if (chip->subSystemID == 0 && chip->subSystemVendorID == 0) {
         /* When subSystemID and subSystemVendorID are both 0, this is the
          * default name for the given chip.  A more specific name may exist
          * elsewhere in the list.
          */
         assert(name == NULL);
         name = chip->name;
         continue;
      }

      /* If we find a specific name, return it */
      if (chip->subSystemID == subsystem_id &&
          chip->subSystemVendorID == subsystem_vendor_id)
         return chip->name;
   }

   return name;
}

uint8_t
sm_for_chipset(uint16_t chipset)
{
   if (chipset >= 0x1b0)
      return 120;
   else if (chipset >= 0x1a0)
      return 100;
   else if (chipset >= 0x190)
      return 89;
   // GH100 is older than AD10X, but is SM90
   else if (chipset >= 0x180)
      return 90;
   else if (chipset == 0x17b)
      return 87;
   else if (chipset >= 0x172)
      return 86;
   else if (chipset >= 0x170)
      return 80;
   else if (chipset >= 0x160)
      return 75;
   else if (chipset >= 0x14b)
      return 72;
   else if (chipset >= 0x140)
      return 70;
   else if (chipset >= 0x13b)
      return 62;
   else if (chipset >= 0x132)
      return 61;
   else if (chipset >= 0x130)
      return 60;
   else if (chipset >= 0x12b)
      return 53;
   else if (chipset >= 0x120)
      return 52;
   else if (chipset >= 0x110)
      return 50;
   // TODO: 37
   else if (chipset >= 0x0f0)
      return 35;
   else if (chipset >= 0x0ea)
      return 32;
   else if (chipset >= 0x0e0)
      return 30;
   // GF110 is SM20
   else if (chipset == 0x0c8)
      return 20;
   else if (chipset >= 0x0c1)
      return 21;
   else if (chipset >= 0x0c0)
      return 20;
   else if (chipset >= 0x0a3)
      return 12;
   // GT200 is SM13
   else if (chipset >= 0x0a0)
      return 13;
   else if (chipset >= 0x080)
      return 11;
   // this has to be == because 0x63 is older than 0x50 and has no compute
   else if (chipset == 0x050)
      return 10;
   // no compute
   return 0x00;
}

uint8_t
max_warps_per_mp_for_sm(uint8_t sm)
{
   /* These are documented in each architecture's tuning guide. Eg. see:
    * https://docs.nvidia.com/cuda/blackwell-tuning-guide/index.html#occupancy
    */
   switch (sm) {
   case 10:
   case 11:
      return 24;
   case 12:
   case 13:
   case 75:
      return 32;
   case 20:
   case 21:
   case 86:
   case 87:
   case 89:
   case 120:
      return 48;
   case 30:
   case 32:
   case 35:
   case 37:
   case 50:
   case 52:
   case 53:
   case 60:
   case 61:
   case 62:
   case 70:
   case 72:
   case 80:
   case 90:
   case 100:
   case 104:
      return 64;
   default:
      assert(!"unkown SM version");
      // return the biggest known value
      return 64;
   }
}

uint8_t
max_blocks_per_mp_for_sm(uint8_t sm)
{
   /* Values taken from CUDA programming guide section "Compute Capabilities" */
   switch (sm) {
   case 10:
   case 11:
   case 12:
   case 13:
   case 20:
   case 21:
      return 8;
   case 30:
   case 32:
   case 35:
   case 37:
   case 75:
   case 86:
   case 87:
      return 16;
   case 89:
   case 110:
   case 120:
      return 24;
   case 50:
   case 52:
   case 53:
   case 60:
   case 61:
   case 62:
   case 70:
   case 72:
   case 80:
   case 90:
   case 100:
      return 32;
   default:
      assert(!"unkown SM version");
      /* return the smallest known value */
      return 8;
   }
}

uint8_t
mp_per_tpc_for_chipset(uint16_t chipset)
{
   // GP100 is special and has two, otherwise it's a Volta and newer thing to have two
   if (chipset == 0x130 || chipset >= 0x140)
      return 2;
   return 1;
}

void
init_shared_mem_sizes(struct nv_device_info *info)
{
   if (info->sm >= 80) {
      const uint16_t ampere_shared_mem[10] =
         { 0, 8, 16, 32, 64, 100, 132, 164, 196, 228 };

      /* Quotes taken from current CUDA docs */
      if (info->sm >= 120) {
         /* The docs on this are a bit contradictory, but CUDA tooling reports
          * values in line with the older SM levels reporting up to 100k.
          *
          * For devices of compute capability 12.0, shared memory capacity per
          * SM is 128KB.
          * For devices of compute capability 12.0 the maximum shared memory
          * per thread block is 99 KB.
          */
         info->sm_smem_size_count = 6;
      } else if (info->sm >= 90) {
         /* Both the NVIDIA H100 GPU and the NVIDIA B200 GPU support shared
          * memory capacities of 0, 8, 16, 32, 64, 100, 132, 164, 196 and
          * 228 KB per SM.
          *
          * GB10X has the same limits.
          */
         info->sm_smem_size_count = 10;
      } else if (info->sm == 80 || info->sm == 87) {
         /* The NVIDIA A100 GPU supports shared memory capacity of 0, 8, 16,
          * 32, 64, 100, 132 or 164 KB per SM.
          *
          * Same for GA10B.
          */
         info->sm_smem_size_count = 8;
      } else if (info->sm == 89 || info->sm == 86) {
         /* GPUs with compute capability 8.6 support shared memory capacity of
          * 0, 8, 16, 32, 64 or 100 KB per SM.
          * The NVIDIA Ada GPU architecture supports shared memory capacity of
          * 0, 8, 16, 32, 64 or 100 KB per SM.
          */
         info->sm_smem_size_count = 6;
      } else {
         UNREACHABLE("Unknown shared memory support for SM.");
      }

      assert(info->sm_smem_size_count <= ARRAY_SIZE(ampere_shared_mem));
      typed_memcpy(info->sm_smem_sizes_kB, ampere_shared_mem,
                   info->sm_smem_size_count);
      STATIC_ASSERT(ARRAY_SIZE(ampere_shared_mem) <=
                    ARRAY_SIZE(info->sm_smem_sizes_kB));
   } else if (info->sm >= 75) {
      /* https://docs.nvidia.com/cuda/turing-tuning-guide/index.html#unified-shared-memory-l1-texture-cache
       *   Turing supports two carveout configurations, either with 64 KB of
       *   shared memory and 32 KB of L1, or with 32 KB of shared memory and
       *   64 KB of L1. Turing allows a single thread block to address the full
       *   64 KB of shared memory.
       */
      info->sm_smem_sizes_kB[0] = 32;
      info->sm_smem_sizes_kB[1] = 64;
      info->sm_smem_size_count = 2;
   } else if (info->sm >= 70) {
      /* https://docs.nvidia.com/cuda/archive/12.9.1/volta-tuning-guide/index.html#unified-shared-memory-l1-texture-cache
       *
       *   Volta supports shared memory capacities of 0, 8, 16, 32, 64, or
       *   96 KB per SM. A new feature, Volta enables a single thread block
       *   to address the full 96 KB of shared memory.
       */
      const uint16_t volta_shared_mem[6] = { 0, 8, 16, 32, 64, 96 };
      info->sm_smem_size_count = ARRAY_SIZE(volta_shared_mem);
      typed_memcpy(info->sm_smem_sizes_kB, volta_shared_mem,
                   info->sm_smem_size_count);
      STATIC_ASSERT(ARRAY_SIZE(volta_shared_mem) <=
                    ARRAY_SIZE(info->sm_smem_sizes_kB));
   } else if (info->sm >= 50) {
      /* https://docs.nvidia.com/cuda/archive/12.9.1/maxwell-tuning-guide/index.html#shared-memory-capacity
       *
       *   GM107 provides 64 KB shared memory per SMM, and GM204 further
       *   increases this to 96 KB shared memory per SMM.
       *
       * https://docs.nvidia.com/cuda/archive/12.9.1/pascal-tuning-guide/index.html#shared-memory-capacity
       *
       *   GP100 offers 64 KB shared memory per SM, and GP104 provides 96 KB per SM.
       *
       * Limits for Tegra (SM53, SM62) are taken from the now gone occupancy
       * calculator.
       */
      info->sm_smem_size_count = 1;
      switch (info->sm) {
      case 50:
      case 53:
      case 60:
      case 62:
         info->sm_smem_sizes_kB[0] = 64;
         break;
      case 52:
      case 61:
         info->sm_smem_sizes_kB[0] = 96;
         break;
      default:
         UNREACHABLE("unknown shared mem size for sm");
         break;
      }
   } else if (info->sm == 37) {
      /* https://docs.nvidia.com/cuda/archive/11.8.0/kepler-tuning-guide/index.html#shared-memory-and-warp-shuffle:
       *
       *   GK210 improves on this by increasing the shared memory capacity per
       *   multiprocessor for each of the configurations described above by a
       *   further 64 (i.e., the application can select 112 KB, 96 KB, or 80
       *   KB of shared memory).
       *
       *   Note: The maximum shared memory per thread block for all Kepler
       *   GPUs, including GK210, remains 48 KB.
       */
      info->sm_smem_sizes_kB[0] = 80;
      info->sm_smem_sizes_kB[1] = 96;
      info->sm_smem_sizes_kB[2] = 112;
      info->sm_smem_size_count = 3;
   } else if (info->sm >= 30) {
      /* NVA0C0_QMDV00_06_L1_CONFIGURATION */
      info->sm_smem_sizes_kB[0] = 16;
      info->sm_smem_sizes_kB[1] = 32;
      info->sm_smem_sizes_kB[2] = 48;
      info->sm_smem_size_count = 3;
   } else if (info->sm >= 20) {
      /* NV90C0_SET_L1_CONFIGURATION */
      info->sm_smem_sizes_kB[0] = 16;
      info->sm_smem_sizes_kB[1] = 48;
      info->sm_smem_size_count = 2;
   } else {
      info->sm_smem_sizes_kB[0] = 16;
      info->sm_smem_size_count = 1;
   }

   /* See above, despite having more shared memory available, Kepler up to
    * Pascal can only address up to 48kB per workgroup.
    */
   if (info->sm < 70 && info->sm >= 30) {
      info->max_smem_per_wg_kB = 48;
   } else {
      info->max_smem_per_wg_kB =
         info->sm_smem_sizes_kB[info->sm_smem_size_count - 1];
   }
}
