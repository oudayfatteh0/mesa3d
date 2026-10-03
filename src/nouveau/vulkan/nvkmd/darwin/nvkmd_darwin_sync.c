/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "util/os_time.h"
#include "util/u_atomic.h"
#include "vk_device.h"
#include "vk_log.h"

#include <sched.h>
#include <string.h>

#define NVKMD_DARWIN_SEM_CHUNK_B (64u * 1024)

struct nvkmd_darwin_sem_chunk {
   struct nvrm_dma dma;
   uint64_t addr;
};

VkResult
nvkmd_darwin_backoff(struct nvkmd_darwin_pdev *pdev, uint32_t *iter)
{
   if (*iter < 64)
      sched_yield();
   else
      os_time_sleep(MIN2(*iter - 63, 20) * 10);

   int err = 0;
   if ((*iter & 1023) == 1023) {
      simple_mtx_lock(&pdev->rm_mutex);
      err = nvrm_dev_poll(pdev->rm);
      simple_mtx_unlock(&pdev->rm_mutex);
   }

   (*iter)++;

   return err == 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static VkResult
sem_grow_locked(struct nvkmd_darwin_pdev *pdev,
                struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_sem_chunk chunk = { 0 };

   const uint32_t slot_count = NVKMD_DARWIN_SEM_CHUNK_B / sizeof(uint64_t);
   if (util_dynarray_ensure_cap(&pdev->sem_chunks,
                                pdev->sem_chunks.size +
                                sizeof(struct nvkmd_darwin_sem_chunk)) == NULL ||
       util_dynarray_ensure_cap(&pdev->sem_free,
                                pdev->sem_free.size +
                                slot_count * sizeof(struct nvkmd_darwin_sem)) ==
          NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   simple_mtx_lock(&pdev->heap_mutex);
   chunk.addr = util_vma_heap_alloc(&pdev->heap, NVKMD_DARWIN_SEM_CHUNK_B,
                                    NVKMD_DARWIN_SEM_CHUNK_B);
   simple_mtx_unlock(&pdev->heap_mutex);
   if (chunk.addr == 0)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate virtual address range");

   simple_mtx_lock(&pdev->rm_mutex);
   int err = nvrm_sysmem_alloc(pdev->rm, NVKMD_DARWIN_SEM_CHUNK_B, &chunk.dma);
   if (err == 0) {
      err = nvkmd_darwin_map_sysmem_locked(pdev, chunk.addr, &chunk.dma, 0,
                                           NVKMD_DARWIN_SEM_CHUNK_B, 0);
      if (err != 0)
         nvrm_sysmem_free(pdev->rm, &chunk.dma);
   }
   simple_mtx_unlock(&pdev->rm_mutex);

   if (err != 0) {
      simple_mtx_lock(&pdev->heap_mutex);
      util_vma_heap_free(&pdev->heap, chunk.addr, NVKMD_DARWIN_SEM_CHUNK_B);
      simple_mtx_unlock(&pdev->heap_mutex);
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate semaphore memory: %s",
                       strerror(-err));
   }

   memset(chunk.dma.cpu, 0, NVKMD_DARWIN_SEM_CHUNK_B);
   util_dynarray_append(&pdev->sem_chunks, chunk);

   volatile uint64_t *map = chunk.dma.cpu;
   for (uint32_t i = slot_count; i-- > 0;) {
      struct nvkmd_darwin_sem sem = {
         .map = &map[i],
         .addr = chunk.addr + i * sizeof(uint64_t),
      };
      util_dynarray_append(&pdev->sem_free, sem);
   }

   return VK_SUCCESS;
}

VkResult
nvkmd_darwin_sem_alloc(struct nvkmd_darwin_pdev *pdev,
                       struct vk_object_base *log_obj,
                       struct nvkmd_darwin_sem *sem)
{
   simple_mtx_lock(&pdev->sem_mutex);
   if (util_dynarray_num_elements(&pdev->sem_free,
                                  struct nvkmd_darwin_sem) == 0) {
      VkResult result = sem_grow_locked(pdev, log_obj);
      if (result != VK_SUCCESS) {
         simple_mtx_unlock(&pdev->sem_mutex);
         return result;
      }
   }
   *sem = util_dynarray_pop(&pdev->sem_free, struct nvkmd_darwin_sem);
   simple_mtx_unlock(&pdev->sem_mutex);

   return VK_SUCCESS;
}

void
nvkmd_darwin_sem_free(struct nvkmd_darwin_pdev *pdev,
                      const struct nvkmd_darwin_sem *sem)
{
   simple_mtx_lock(&pdev->sem_mutex);
   util_dynarray_append(&pdev->sem_free, *sem);
   simple_mtx_unlock(&pdev->sem_mutex);
}

void
nvkmd_darwin_sem_finish(struct nvkmd_darwin_pdev *pdev)
{
   util_dynarray_foreach(&pdev->sem_chunks, struct nvkmd_darwin_sem_chunk,
                         chunk) {
      simple_mtx_lock(&pdev->rm_mutex);
      nvrm_vm_unmap(pdev->rm, chunk->addr, NVKMD_DARWIN_SEM_CHUNK_B);
      nvrm_sysmem_free(pdev->rm, &chunk->dma);
      simple_mtx_unlock(&pdev->rm_mutex);

      simple_mtx_lock(&pdev->heap_mutex);
      util_vma_heap_free(&pdev->heap, chunk->addr, NVKMD_DARWIN_SEM_CHUNK_B);
      simple_mtx_unlock(&pdev->heap_mutex);
   }
   util_dynarray_fini(&pdev->sem_chunks);
   util_dynarray_fini(&pdev->sem_free);
}

VkResult
nvkmd_darwin_sem_wait(struct nvkmd_darwin_pdev *pdev,
                      const struct nvkmd_darwin_sem *sem, uint64_t value)
{
   uint32_t iter = 0;
   while (*sem->map < value) {
      VkResult result = nvkmd_darwin_backoff(pdev, &iter);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

void
nvkmd_darwin_sync_set_pending(struct nvkmd_darwin_sync *sync, uint64_t value)
{
   uint64_t old = p_atomic_read(&sync->pending);
   while (old < value) {
      const uint64_t prev = p_atomic_cmpxchg(&sync->pending, old, value);
      if (prev == old)
         break;
      old = prev;
   }
}

static struct nvkmd_darwin_pdev *
sync_pdev(struct vk_sync *sync)
{
   return container_of(sync->type, struct nvkmd_darwin_pdev, sync_type);
}

static VkResult
nvkmd_darwin_sync_init(struct vk_device *device,
                       struct vk_sync *_sync,
                       uint64_t initial_value)
{
   struct nvkmd_darwin_sync *sync = nvkmd_darwin_sync(_sync);

   VkResult result = nvkmd_darwin_sem_alloc(sync_pdev(_sync), &device->base,
                                            &sync->sem);
   if (result != VK_SUCCESS)
      return result;

   *sync->sem.map = initial_value;
   sync->pending = initial_value;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_sync_finish(struct vk_device *device, struct vk_sync *_sync)
{
   struct nvkmd_darwin_sync *sync = nvkmd_darwin_sync(_sync);

   nvkmd_darwin_sem_free(sync_pdev(_sync), &sync->sem);
}

static VkResult
nvkmd_darwin_sync_signal(struct vk_device *device,
                         struct vk_sync *_sync,
                         uint64_t value)
{
   struct nvkmd_darwin_sync *sync = nvkmd_darwin_sync(_sync);

   nvkmd_darwin_sync_set_pending(sync, value);
   *sync->sem.map = value;

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_sync_get_value(struct vk_device *device,
                            struct vk_sync *_sync,
                            uint64_t *value)
{
   struct nvkmd_darwin_sync *sync = nvkmd_darwin_sync(_sync);

   *value = *sync->sem.map;

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_sync_wait_many(struct vk_device *device,
                            uint32_t wait_count,
                            const struct vk_sync_wait *waits,
                            enum vk_sync_wait_flags wait_flags,
                            uint64_t abs_timeout_ns)
{
   uint32_t iter = 0;
   for (;;) {
      uint32_t done = 0;
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_darwin_sync *sync = nvkmd_darwin_sync(waits[i].sync);

         uint64_t value = *sync->sem.map;
         if (wait_flags & VK_SYNC_WAIT_PENDING)
            value = MAX2(value, p_atomic_read(&sync->pending));

         if (value >= waits[i].wait_value) {
            if (wait_flags & VK_SYNC_WAIT_ANY)
               return VK_SUCCESS;
            done++;
         }
      }

      if (done == wait_count)
         return VK_SUCCESS;

      if (os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;

      VkResult result = nvkmd_darwin_backoff(sync_pdev(waits[0].sync), &iter);
      if (result != VK_SUCCESS)
         return vk_device_set_lost(device, "GSP reported a GPU error");
   }
}

void
nvkmd_darwin_init_sync_types(struct nvkmd_darwin_pdev *pdev)
{
   pdev->sync_type = (struct vk_sync_type) {
      .size = sizeof(struct nvkmd_darwin_sync),
      .features = VK_SYNC_FEATURE_TIMELINE |
                  VK_SYNC_FEATURE_GPU_WAIT |
                  VK_SYNC_FEATURE_CPU_WAIT |
                  VK_SYNC_FEATURE_CPU_SIGNAL |
                  VK_SYNC_FEATURE_WAIT_ANY |
                  VK_SYNC_FEATURE_WAIT_PENDING |
                  VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
      .init = nvkmd_darwin_sync_init,
      .finish = nvkmd_darwin_sync_finish,
      .signal = nvkmd_darwin_sync_signal,
      .get_value = nvkmd_darwin_sync_get_value,
      .wait_many = nvkmd_darwin_sync_wait_many,
   };
   pdev->sync_binary_type = vk_sync_binary_get_type(&pdev->sync_type);

   pdev->sync_types[0] = &pdev->sync_type;
   pdev->sync_types[1] = &pdev->sync_binary_type.sync;
   pdev->sync_types[2] = NULL;
}
