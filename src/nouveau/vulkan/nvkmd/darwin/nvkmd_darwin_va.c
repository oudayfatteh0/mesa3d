/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int
nvkmd_darwin_map_sysmem_locked(struct nvkmd_darwin_pdev *pdev,
                               uint64_t addr, const struct nvrm_dma *dma,
                               uint64_t offset_B, uint64_t range_B,
                               uint8_t pte_kind)
{
   assert(offset_B % NVKMD_DARWIN_PAGE_B == 0);
   assert(range_B % NVKMD_DARWIN_PAGE_B == 0);

   const uint64_t first = offset_B / NVKMD_DARWIN_PAGE_B;
   const uint64_t count = range_B / NVKMD_DARWIN_PAGE_B;
   assert(first + count <= dma->npages);

   struct nvrm_range *ranges = malloc(count * sizeof(*ranges));
   if (ranges == NULL)
      return -ENOMEM;

   uint32_t range_count = 0;
   for (uint64_t i = 0; i < count; i++) {
      const uint64_t page = dma->pages[first + i];
      struct nvrm_range *prev = range_count ? &ranges[range_count - 1] : NULL;
      if (prev != NULL && prev->addr + prev->size == page)
         prev->size += NVKMD_DARWIN_PAGE_B;
      else
         ranges[range_count++] = (struct nvrm_range) {
            .addr = page,
            .size = NVKMD_DARWIN_PAGE_B,
         };
   }

   int err = nvrm_vm_map(pdev->rm, addr, ranges, range_count,
                         NVRM_APERTURE_SYSMEM, pte_kind, NVRM_MAP_UNCACHED);
   free(ranges);

   return err;
}

static VkResult MUST_CHECK
alloc_heap_addr_locked(struct nvkmd_darwin_pdev *pdev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, uint64_t *addr_out)
{
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      assert(flags & NVKMD_VA_REPLAY);
      if (fixed_addr < NVKMD_DARWIN_REPLAY_HEAP_START ||
          fixed_addr >= NVKMD_DARWIN_REPLAY_HEAP_END) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Capture address 0x%" PRIx64 " not in the replay "
                          "heap address range [0x%" PRIx64 ", 0x%" PRIx64 ")",
                          fixed_addr, NVKMD_DARWIN_REPLAY_HEAP_START,
                          NVKMD_DARWIN_REPLAY_HEAP_END);
      }

      if (fixed_addr & (align_B - 1)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Unaligned capture address: 0x%" PRIx64, fixed_addr);
      }

      if (!util_vma_heap_alloc_addr(&pdev->replay_heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Replay address collision: 0x%" PRIx64, fixed_addr);
      }

      *addr_out = fixed_addr;
   } else {
      struct util_vma_heap *heap =
         (flags & NVKMD_VA_REPLAY) ? &pdev->replay_heap : &pdev->heap;
      *addr_out = util_vma_heap_alloc(heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   }

   return VK_SUCCESS;
}

static void
free_heap_addr(struct nvkmd_darwin_pdev *pdev,
               enum nvkmd_va_flags flags,
               uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&pdev->heap_mutex);
   if (flags & NVKMD_VA_REPLAY) {
      assert(addr >= NVKMD_DARWIN_REPLAY_HEAP_START);
      assert(addr + size_B <= NVKMD_DARWIN_REPLAY_HEAP_END);
      util_vma_heap_free(&pdev->replay_heap, addr, size_B);
   } else {
      assert(addr >= NVKMD_DARWIN_HEAP_START);
      assert(addr + size_B <= NVKMD_DARWIN_HEAP_END);
      util_vma_heap_free(&pdev->heap, addr, size_B);
   }
   simple_mtx_unlock(&pdev->heap_mutex);
}

VkResult
nvkmd_darwin_alloc_va(struct nvkmd_dev *dev,
                      struct vk_object_base *log_obj,
                      enum nvkmd_va_flags flags, uint8_t pte_kind,
                      uint64_t size_B, uint64_t align_B,
                      uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(dev->pdev);

   struct nvkmd_darwin_va *va = CALLOC_STRUCT(nvkmd_darwin_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, pdev->base.bind_align_B);
   size_B = align64(size_B, align_B);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   simple_mtx_lock(&pdev->heap_mutex);
   VkResult result = alloc_heap_addr_locked(pdev, log_obj, flags, size_B,
                                            align_B, fixed_addr,
                                            &va->base.addr);
   simple_mtx_unlock(&pdev->heap_mutex);
   if (result != VK_SUCCESS) {
      FREE(va);
      return result;
   }

   va->base.ops = &nvkmd_darwin_va_ops;
   va->base.dev = dev;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

   *va_out = &va->base;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_va->dev->pdev);
   struct nvkmd_darwin_va *va = nvkmd_darwin_va(_va);

   simple_mtx_lock(&pdev->rm_mutex);
   int err = nvrm_vm_unmap(pdev->rm, va->base.addr, va->base.size_B);
   simple_mtx_unlock(&pdev->rm_mutex);

   if (err == 0)
      free_heap_addr(pdev, va->base.flags, va->base.addr, va->base.size_B);

   FREE(va);
}

static VkResult
nvkmd_darwin_va_bind_mem(struct nvkmd_va *_va,
                         struct vk_object_base *log_obj,
                         uint64_t va_offset_B,
                         struct nvkmd_mem *_mem,
                         uint64_t mem_offset_B,
                         uint64_t range_B)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_va->dev->pdev);
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);

   assert(_mem->dev == _va->dev);

   const uint64_t addr = _va->addr + va_offset_B;

   simple_mtx_lock(&pdev->rm_mutex);
   int err = nvrm_vm_unmap(pdev->rm, addr, range_B);
   if (err == 0) {
      if (mem->vram) {
         const struct nvrm_range range = {
            .addr = mem->paddr + mem_offset_B,
            .size = range_B,
         };
         err = nvrm_vm_map(pdev->rm, addr, &range, 1, NVRM_APERTURE_VRAM,
                           _va->pte_kind, 0);
      } else {
         err = nvkmd_darwin_map_sysmem_locked(pdev, addr, &mem->dma,
                                              mem_offset_B, range_B,
                                              _va->pte_kind);
      }
   }
   simple_mtx_unlock(&pdev->rm_mutex);

   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to map 0x%" PRIx64 "+0x%" PRIx64 ": %s",
                       addr, range_B, strerror(-err));

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_va_unbind(struct nvkmd_va *_va,
                       struct vk_object_base *log_obj,
                       uint64_t va_offset_B,
                       uint64_t range_B)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_va->dev->pdev);

   const uint64_t addr = _va->addr + va_offset_B;

   simple_mtx_lock(&pdev->rm_mutex);
   int err = nvrm_vm_unmap(pdev->rm, addr, range_B);
   simple_mtx_unlock(&pdev->rm_mutex);

   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "Failed to unmap 0x%" PRIx64 "+0x%" PRIx64 ": %s",
                       addr, range_B, strerror(-err));

   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_darwin_va_ops = {
   .free = nvkmd_darwin_va_free,
   .bind_mem = nvkmd_darwin_va_bind_mem,
   .unbind = nvkmd_darwin_va_unbind,
};
