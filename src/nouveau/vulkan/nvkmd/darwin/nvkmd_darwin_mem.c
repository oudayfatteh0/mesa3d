/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <errno.h>
#include <string.h>

static void
free_backing(struct nvkmd_darwin_pdev *pdev, struct nvkmd_darwin_mem *mem)
{
   simple_mtx_lock(&pdev->rm_mutex);
   if (mem->vram)
      nvrm_vram_free(pdev->rm, mem->paddr, mem->base.size_B);
   else
      nvrm_sysmem_free(pdev->rm, &mem->dma);
   simple_mtx_unlock(&pdev->rm_mutex);
}

static VkResult
alloc_backing(struct nvkmd_darwin_pdev *pdev,
              struct vk_object_base *log_obj,
              struct nvkmd_darwin_mem *mem,
              uint64_t size_B, uint64_t align_B,
              enum nvkmd_mem_flags flags)
{
   int err = -ENOMEM;

   const bool try_vram = (flags & (NVKMD_MEM_LOCAL | NVKMD_MEM_VRAM)) &&
                         !(pdev->base.debug_flags & NVK_DEBUG_FORCE_GART);
   if (try_vram) {
      const uint32_t rm_flags =
         (flags & NVKMD_MEM_CAN_MAP) ? NVRM_VRAM_MAPPABLE : 0;

      simple_mtx_lock(&pdev->rm_mutex);
      err = nvrm_vram_alloc(pdev->rm, size_B, align_B, rm_flags, &mem->paddr);
      if (err == 0 && (flags & NVKMD_MEM_CAN_MAP)) {
         mem->map = (void *)nvrm_vram_cpu(pdev->rm, mem->paddr, size_B);
         assert(mem->map != NULL);
      }
      simple_mtx_unlock(&pdev->rm_mutex);

      if (err == 0) {
         mem->vram = true;
         return VK_SUCCESS;
      }

      if ((flags & NVKMD_MEM_VRAM) &&
          !(pdev->base.debug_flags & NVK_DEBUG_FORCE_GART))
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate VRAM: %s", strerror(-err));
   }

   simple_mtx_lock(&pdev->rm_mutex);
   err = nvrm_sysmem_alloc(pdev->rm, size_B, &mem->dma);
   simple_mtx_unlock(&pdev->rm_mutex);

   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate system memory: %s",
                       strerror(-err));

   mem->vram = false;
   mem->map = mem->dma.cpu;

   return VK_SUCCESS;
}

VkResult
nvkmd_darwin_alloc_mem(struct nvkmd_dev *dev,
                       struct vk_object_base *log_obj,
                       uint64_t size_B, uint64_t align_B,
                       enum nvkmd_mem_flags flags,
                       struct nvkmd_mem **mem_out)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(dev->pdev);
   VkResult result;

   assert(util_bitcount(flags & NVKMD_MEM_PLACEMENT_FLAGS) == 1);
   assert(util_is_power_of_two_or_zero64(align_B));

   align_B = MAX2(align_B, pdev->base.bind_align_B);
   if (!(flags & NVKMD_MEM_GART) && size_B >= NVKMD_DARWIN_BIG_PAGE_B)
      align_B = MAX2(align_B, NVKMD_DARWIN_BIG_PAGE_B);
   size_B = align64(size_B, align_B);

   struct nvkmd_darwin_mem *mem = CALLOC_STRUCT(nvkmd_darwin_mem);
   if (mem == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = alloc_backing(pdev, log_obj, mem, size_B, align_B, flags);
   if (result != VK_SUCCESS) {
      FREE(mem);
      return result;
   }

   nvkmd_mem_init(dev, &mem->base, &nvkmd_darwin_mem_ops,
                  flags | NVKMD_MEM_COHERENT, size_B,
                  pdev->base.bind_align_B);

   result = nvkmd_dev_alloc_va(dev, log_obj, mem->vram ? 0 : NVKMD_VA_GART,
                               0, size_B, align_B, 0, &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_backing;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base, 0,
                              size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_backing:
   free_backing(pdev, mem);
   FREE(mem);

   return result;
}

static void
nvkmd_darwin_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_mem->dev->pdev);
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);

   nvkmd_va_free(mem->base.va);
   free_backing(pdev, mem);
   FREE(mem);
}

static VkResult
nvkmd_darwin_mem_map(struct nvkmd_mem *_mem,
                     struct vk_object_base *log_obj,
                     enum nvkmd_mem_map_flags flags,
                     void *fixed_addr,
                     void **map_out)
{
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);

   assert(!(flags & NVKMD_MEM_MAP_FIXED));

   if (mem->map == NULL)
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory is not CPU visible");

   *map_out = mem->map;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_mem_unmap(struct nvkmd_mem *_mem,
                       enum nvkmd_mem_map_flags flags,
                       void *map)
{
}

static uint32_t
nvkmd_darwin_mem_log_handle(struct nvkmd_mem *_mem)
{
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);

   return mem->vram ? mem->paddr / NVKMD_DARWIN_PAGE_B : mem->dma.handle;
}

const struct nvkmd_mem_ops nvkmd_darwin_mem_ops = {
   .free = nvkmd_darwin_mem_free,
   .map = nvkmd_darwin_mem_map,
   .unmap = nvkmd_darwin_mem_unmap,
   .log_handle = nvkmd_darwin_mem_log_handle,
};
