/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "util/u_memory.h"
#include "vk_log.h"

VkResult
nvkmd_darwin_create_dev(struct nvkmd_pdev *_pdev,
                        struct vk_object_base *log_obj,
                        struct nvkmd_dev **dev_out)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_pdev);

   struct nvkmd_darwin_dev *dev = CALLOC_STRUCT(nvkmd_darwin_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_darwin_dev_ops;
   dev->base.pdev = &pdev->base;
   dev->base.va_start = 0;
   dev->base.va_end = NVKMD_DARWIN_REPLAY_HEAP_END;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);

   *dev_out = &dev->base;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
}

static uint64_t
nvkmd_darwin_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_dev->pdev);

   return nvrm_gpu_timestamp(pdev->rm);
}

const struct nvkmd_dev_ops nvkmd_darwin_dev_ops = {
   .destroy = nvkmd_darwin_dev_destroy,
   .get_gpu_timestamp = nvkmd_darwin_dev_get_gpu_timestamp,
   .alloc_mem = nvkmd_darwin_alloc_mem,
   .alloc_va = nvkmd_darwin_alloc_va,
   .create_ctx = nvkmd_darwin_create_ctx,
};
