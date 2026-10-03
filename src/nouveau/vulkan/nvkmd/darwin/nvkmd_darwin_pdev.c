/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "nouveau_device_info.h"
#include "util/cache_ops.h"
#include "util/log.h"
#include "util/os_misc.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvtypes.h"
#include "cl902d.h"
#include "cla040.h"
#include "cla06f.h"
#include "cla097.h"
#include "cla0b5.h"
#include "cla0c0.h"

static void
nvkmd_darwin_rm_log(void *ctx, int level, const char *msg)
{
   if (level == 0)
      mesa_loge("nvrm: %s", msg);
   else
      mesa_logi("nvrm: %s", msg);
}

static uint16_t
find_class(const struct nvrm_dev_info *rm, uint8_t cls_lo, uint32_t min_cls)
{
   uint32_t best = 0;
   for (uint32_t i = 0; i < rm->class_count; i++) {
      const uint32_t cls = rm->classes[i];
      if ((cls & 0xff) == cls_lo && cls >= min_cls && cls <= UINT16_MAX &&
          cls > best)
         best = cls;
   }
   return best;
}

static void
fill_dev_info(struct nv_device_info *info, const struct nvrm_dev_info *rm)
{
   *info = (struct nv_device_info) {
      .type = NV_DEVICE_TYPE_DIS,
      .device_id = rm->pci_device_id,
      .chipset = rm->chipset,
      .pci = {
         .domain = 0,
         .bus = rm->pci_bdf >> 8,
         .dev = (rm->pci_bdf >> 3) & 0x1f,
         .func = rm->pci_bdf & 0x7,
         .revision_id = rm->pci_revision_id,
      },
      .gpc_count = rm->gpc_count,
      .tpc_count = rm->tpc_count,
      .nc_atom_size_B = util_cache_granularity(),
      .cls_copy = find_class(rm, 0xb5, KEPLER_DMA_COPY_A),
      .cls_eng2d = find_class(rm, 0x2d, FERMI_TWOD_A),
      .cls_eng3d = find_class(rm, 0x97, KEPLER_A),
      .cls_m2mf = find_class(rm, 0x40, KEPLER_INLINE_TO_MEMORY_A),
      .cls_compute = find_class(rm, 0xc0, KEPLER_COMPUTE_A),
      .cls_gpfifo = find_class(rm, 0x6f, KEPLER_CHANNEL_GPFIFO_A),
      .vram_size_B = rm->vram_size,
      .bar_size_B = rm->bar1_size,
   };

   STATIC_ASSERT(sizeof(info->chipset_name) >= sizeof(rm->chip_name));
   memcpy(info->chipset_name, rm->chip_name, sizeof(rm->chip_name));

   const char *name = name_for_chip(rm->pci_device_id, rm->pci_subsystem_id,
                                    rm->pci_subsystem_vendor_id);
   if (name != NULL)
      snprintf(info->device_name, sizeof(info->device_name), "%s", name);
   else
      snprintf(info->device_name, sizeof(info->device_name), "NVIDIA %s",
               rm->chip_name);

   info->sm = sm_for_chipset(info->chipset);
   info->max_warps_per_mp = max_warps_per_mp_for_sm(info->sm);
   info->max_blocks_per_mp = max_blocks_per_mp_for_sm(info->sm);
   info->mp_per_tpc = mp_per_tpc_for_chipset(info->chipset);
   init_shared_mem_sizes(info);
}

static void
get_fw_dir(char *buf, size_t size)
{
   const char *dir = os_get_option("MACNV_FIRMWARE_DIR");
   if (dir != NULL) {
      snprintf(buf, size, "%s", dir);
   } else {
      const char *home = getenv("HOME");
      snprintf(buf, size, "%s/Library/Application Support/macnv/firmware",
               home != NULL ? home : "");
   }
}

VkResult
nvkmd_darwin_try_create_pdev(struct vk_object_base *log_obj,
                             enum nvk_debug debug_flags,
                             struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_darwin_pdev *pdev = CALLOC_STRUCT(nvkmd_darwin_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (nvrm_platform_macos_open(&pdev->plat) != 0) {
      FREE(pdev);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   pdev->plat.log = nvkmd_darwin_rm_log;
   pdev->plat.log_level = debug_get_num_option("NVKMD_DARWIN_RM_LOG", 0);

   char fw_dir[1024];
   get_fw_dir(fw_dir, sizeof(fw_dir));

   int ret = nvrm_dev_open(&pdev->plat, fw_dir, &pdev->rm);
   if (ret != 0) {
      nvrm_platform_macos_close(&pdev->plat);
      FREE(pdev);
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to boot GSP-RM: %s", strerror(-ret));
   }

   pdev->base.ops = &nvkmd_darwin_pdev_ops;
   pdev->base.debug_flags = debug_flags;
   fill_dev_info(&pdev->base.dev_info, nvrm_dev_info(pdev->rm));

   uint64_t os_page_size;
   os_get_page_size(&os_page_size);
   assert(os_page_size <= UINT32_MAX);
   pdev->base.bind_align_B = os_page_size;

   simple_mtx_init(&pdev->rm_mutex, mtx_plain);
   simple_mtx_init(&pdev->heap_mutex, mtx_plain);
   simple_mtx_init(&pdev->sem_mutex, mtx_plain);

   STATIC_ASSERT(NVKMD_DARWIN_HEAP_START >= NVRM_VA_START);
   STATIC_ASSERT(NVKMD_DARWIN_HEAP_START < NVRM_VA_RESERVED_START);
   STATIC_ASSERT(NVRM_VA_RESERVED_END < NVKMD_DARWIN_HEAP_END);
   util_vma_heap_init(&pdev->heap, NVKMD_DARWIN_HEAP_START,
                      NVKMD_DARWIN_HEAP_END - NVKMD_DARWIN_HEAP_START);
   ASSERTED bool reserved =
      util_vma_heap_alloc_addr(&pdev->heap, NVRM_VA_RESERVED_START,
                               NVRM_VA_RESERVED_END - NVRM_VA_RESERVED_START);
   assert(reserved);

   STATIC_ASSERT(NVKMD_DARWIN_REPLAY_HEAP_END <= (1ull << 40));
   util_vma_heap_init(&pdev->replay_heap, NVKMD_DARWIN_REPLAY_HEAP_START,
                      NVKMD_DARWIN_REPLAY_HEAP_END -
                      NVKMD_DARWIN_REPLAY_HEAP_START);

   util_dynarray_init(&pdev->sem_chunks, NULL);
   util_dynarray_init(&pdev->sem_free, NULL);

   nvkmd_darwin_init_sync_types(pdev);
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_pdev);

   nvkmd_darwin_sem_finish(pdev);
   util_vma_heap_finish(&pdev->heap);
   util_vma_heap_finish(&pdev->replay_heap);
   simple_mtx_destroy(&pdev->sem_mutex);
   simple_mtx_destroy(&pdev->heap_mutex);
   simple_mtx_destroy(&pdev->rm_mutex);

   nvrm_dev_close(pdev->rm);
   nvrm_platform_macos_close(&pdev->plat);
   FREE(pdev);
}

static uint64_t
nvkmd_darwin_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   return 0;
}

static int
nvkmd_darwin_pdev_get_drm_primary_fd(struct nvkmd_pdev *_pdev)
{
   return -1;
}

const struct nvkmd_pdev_ops nvkmd_darwin_pdev_ops = {
   .destroy = nvkmd_darwin_pdev_destroy,
   .get_vram_used = nvkmd_darwin_pdev_get_vram_used,
   .get_drm_primary_fd = nvkmd_darwin_pdev_get_drm_primary_fd,
   .create_dev = nvkmd_darwin_create_dev,
};
