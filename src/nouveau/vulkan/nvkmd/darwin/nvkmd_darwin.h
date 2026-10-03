/*
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_DARWIN_H
#define NVKMD_DARWIN_H 1

#include "nvkmd/nvkmd.h"

#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/vma.h"
#include "vk_sync.h"
#include "vk_sync_binary.h"

#include <libnvrm.h>

#define NVKMD_DARWIN_PAGE_B ((uint64_t)4096)
#define NVKMD_DARWIN_BIG_PAGE_B ((uint64_t)2 << 20)

#define NVKMD_DARWIN_HEAP_START ((uint64_t)4096)
#define NVKMD_DARWIN_HEAP_END ((uint64_t)1 << 38)
#define NVKMD_DARWIN_REPLAY_HEAP_START NVKMD_DARWIN_HEAP_END
#define NVKMD_DARWIN_REPLAY_HEAP_END ((uint64_t)1 << 39)

struct nvkmd_darwin_sem {
   volatile uint64_t *map;
   uint64_t addr;
};

struct nvkmd_darwin_pdev {
   struct nvkmd_pdev base;

   struct nvrm_platform plat;
   struct nvrm_dev *rm;
   simple_mtx_t rm_mutex;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;

   simple_mtx_t sem_mutex;
   struct util_dynarray sem_chunks;
   struct util_dynarray sem_free;

   struct vk_sync_type sync_type;
   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];
};

NVKMD_DECL_SUBCLASS(pdev, darwin);

struct nvkmd_darwin_dev {
   struct nvkmd_dev base;
};

NVKMD_DECL_SUBCLASS(dev, darwin);

struct nvkmd_darwin_mem {
   struct nvkmd_mem base;

   bool vram;
   uint64_t paddr;
   struct nvrm_dma dma;
   void *map;
};

NVKMD_DECL_SUBCLASS(mem, darwin);

struct nvkmd_darwin_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, darwin);

struct nvkmd_darwin_sync {
   struct vk_sync base;

   struct nvkmd_darwin_sem sem;
   uint64_t pending;
};

static inline struct nvkmd_darwin_sync *
nvkmd_darwin_sync(struct vk_sync *sync)
{
   return container_of(sync, struct nvkmd_darwin_sync, base);
}

VkResult nvkmd_darwin_try_create_pdev(struct vk_object_base *log_obj,
                                      enum nvk_debug debug_flags,
                                      struct nvkmd_pdev **pdev_out);

VkResult nvkmd_darwin_create_dev(struct nvkmd_pdev *pdev,
                                 struct vk_object_base *log_obj,
                                 struct nvkmd_dev **dev_out);

VkResult nvkmd_darwin_alloc_mem(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                uint64_t size_B, uint64_t align_B,
                                enum nvkmd_mem_flags flags,
                                struct nvkmd_mem **mem_out);

VkResult nvkmd_darwin_alloc_va(struct nvkmd_dev *dev,
                               struct vk_object_base *log_obj,
                               enum nvkmd_va_flags flags, uint8_t pte_kind,
                               uint64_t size_B, uint64_t align_B,
                               uint64_t fixed_addr, struct nvkmd_va **va_out);

VkResult nvkmd_darwin_create_ctx(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 enum nvkmd_engines engines,
                                 struct nvkmd_ctx **ctx_out);

int nvkmd_darwin_map_sysmem_locked(struct nvkmd_darwin_pdev *pdev,
                                   uint64_t addr, const struct nvrm_dma *dma,
                                   uint64_t offset_B, uint64_t range_B,
                                   uint8_t pte_kind);

void nvkmd_darwin_init_sync_types(struct nvkmd_darwin_pdev *pdev);

VkResult nvkmd_darwin_sem_alloc(struct nvkmd_darwin_pdev *pdev,
                                struct vk_object_base *log_obj,
                                struct nvkmd_darwin_sem *sem);
void nvkmd_darwin_sem_free(struct nvkmd_darwin_pdev *pdev,
                           const struct nvkmd_darwin_sem *sem);
void nvkmd_darwin_sem_finish(struct nvkmd_darwin_pdev *pdev);
VkResult nvkmd_darwin_sem_wait(struct nvkmd_darwin_pdev *pdev,
                               const struct nvkmd_darwin_sem *sem,
                               uint64_t value);

void nvkmd_darwin_sync_set_pending(struct nvkmd_darwin_sync *sync,
                                   uint64_t value);
VkResult nvkmd_darwin_backoff(struct nvkmd_darwin_pdev *pdev, uint32_t *iter);

#endif
