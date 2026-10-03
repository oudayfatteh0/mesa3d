/*
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "nv_push.h"
#include "nv_push_clc56f.h"
#include "util/u_memory.h"
#include "vk_log.h"
#include "vk_sync_dummy.h"

#include <errno.h>
#include <string.h>

#define NVKMD_DARWIN_GPFIFO_ENTRIES (1u << 14)
#define NVKMD_DARWIN_RING_B (256u * 1024)
#define NVKMD_DARWIN_MAX_RING_USES 1024
#define NVKMD_DARWIN_MAX_SYNCS 256
#define NVKMD_DARWIN_SEM_DW 6

struct nvkmd_darwin_ring_use {
   uint32_t start, end;
   uint64_t seqno;
};

struct nvkmd_darwin_ctx_wait {
   uint64_t addr;
   uint64_t value;
};

struct nvkmd_darwin_ctx_signal {
   struct nvkmd_darwin_sync *sync;
   uint64_t value;
};

struct nvkmd_darwin_exec_ctx {
   struct nvkmd_ctx base;

   struct nvrm_chan *chan;
   uint32_t max_submit;

   struct nvkmd_mem *ring;
   uint32_t ring_head;
   struct nvkmd_darwin_ring_use uses[NVKMD_DARWIN_MAX_RING_USES];
   uint32_t use_first, use_count;

   struct nvkmd_darwin_sem fence;
   uint64_t seqno;

   struct util_dynarray waits;
   struct util_dynarray signals;
   struct util_dynarray pushes;
   struct util_dynarray gpfifo;
};

NVKMD_DECL_SUBCLASS(ctx, darwin_exec);

struct nvkmd_darwin_bind_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, darwin_bind);

static struct nvkmd_darwin_sync *
ctx_sync(struct nvkmd_darwin_pdev *pdev, struct vk_sync *sync)
{
   if (vk_sync_type_is_dummy(sync->type))
      return NULL;

   assert(sync->type == &pdev->sync_type);
   return nvkmd_darwin_sync(sync);
}

static void
push_sem_acquire(struct nv_push *p, uint64_t addr, uint64_t value)
{
   __push_mthd(p, SUBC_NV9097, NVC56F_SEM_ADDR_LO);
   P_NVC56F_SEM_ADDR_LO(p, (addr & UINT32_MAX) >> 2);
   P_NVC56F_SEM_ADDR_HI(p, addr >> 32);
   P_NVC56F_SEM_PAYLOAD_LO(p, value & UINT32_MAX);
   P_NVC56F_SEM_PAYLOAD_HI(p, value >> 32);
   P_NVC56F_SEM_EXECUTE(p, {
      .operation = OPERATION_ACQ_STRICT_GEQ,
      .acquire_switch_tsg = ACQUIRE_SWITCH_TSG_EN,
      .payload_size = PAYLOAD_SIZE_64BIT,
   });
}

static void
push_sem_release(struct nv_push *p, uint64_t addr, uint64_t value)
{
   __push_mthd(p, SUBC_NV9097, NVC56F_SEM_ADDR_LO);
   P_NVC56F_SEM_ADDR_LO(p, (addr & UINT32_MAX) >> 2);
   P_NVC56F_SEM_ADDR_HI(p, addr >> 32);
   P_NVC56F_SEM_PAYLOAD_LO(p, value & UINT32_MAX);
   P_NVC56F_SEM_PAYLOAD_HI(p, value >> 32);
   P_NVC56F_SEM_EXECUTE(p, {
      .operation = OPERATION_RELEASE,
      .release_wfi = RELEASE_WFI_EN,
      .payload_size = PAYLOAD_SIZE_64BIT,
   });
}

static void
ring_retire(struct nvkmd_darwin_exec_ctx *ctx)
{
   const uint64_t done = *ctx->fence.map;
   while (ctx->use_count > 0 && ctx->uses[ctx->use_first].seqno <= done) {
      ctx->use_first = (ctx->use_first + 1) % NVKMD_DARWIN_MAX_RING_USES;
      ctx->use_count--;
   }
}

static bool
ring_fits(struct nvkmd_darwin_exec_ctx *ctx, uint32_t size_B,
          uint32_t *offset_out)
{
   if (ctx->use_count == 0) {
      *offset_out = 0;
      return true;
   }

   if (ctx->use_count == NVKMD_DARWIN_MAX_RING_USES)
      return false;

   const uint32_t head = ctx->ring_head;
   const uint32_t tail = ctx->uses[ctx->use_first].start;
   if (tail < head) {
      if (head + size_B <= NVKMD_DARWIN_RING_B) {
         *offset_out = head;
         return true;
      }
      if (size_B <= tail) {
         *offset_out = 0;
         return true;
      }
      return false;
   }

   if (head + size_B <= tail) {
      *offset_out = head;
      return true;
   }

   return false;
}

static VkResult
ring_alloc(struct nvkmd_darwin_pdev *pdev, struct nvkmd_darwin_exec_ctx *ctx,
           uint32_t size_B, uint32_t *offset_out)
{
   assert(size_B <= NVKMD_DARWIN_RING_B);

   uint32_t offset, iter = 0;
   for (;;) {
      ring_retire(ctx);
      if (ring_fits(ctx, size_B, &offset))
         break;
      VkResult result = nvkmd_darwin_backoff(pdev, &iter);
      if (result != VK_SUCCESS)
         return result;
   }

   ctx->ring_head = offset + size_B;
   *offset_out = offset;

   return VK_SUCCESS;
}

static void
ring_push_use(struct nvkmd_darwin_exec_ctx *ctx, uint32_t offset,
              uint32_t size_B, uint64_t seqno)
{
   assert(ctx->use_count < NVKMD_DARWIN_MAX_RING_USES);
   const uint32_t idx =
      (ctx->use_first + ctx->use_count) % NVKMD_DARWIN_MAX_RING_USES;
   ctx->uses[idx] = (struct nvkmd_darwin_ring_use) {
      .start = offset,
      .end = offset + size_B,
      .seqno = seqno,
   };
   ctx->use_count++;
}

static VkResult
nvkmd_darwin_create_exec_ctx(struct nvkmd_dev *dev,
                             struct vk_object_base *log_obj,
                             enum nvkmd_engines engines,
                             struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(dev->pdev);
   VkResult result;

   if (engines & NVKMD_ENGINE_VDEC)
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd/darwin has no video decode");

   uint32_t rm_engines = 0;
   if (engines & NVKMD_ENGINE_COPY)
      rm_engines |= NVRM_ENGINE_COPY;
   if (engines & NVKMD_ENGINE_2D)
      rm_engines |= NVRM_ENGINE_2D;
   if (engines & NVKMD_ENGINE_3D)
      rm_engines |= NVRM_ENGINE_3D;
   if (engines & NVKMD_ENGINE_M2MF)
      rm_engines |= NVRM_ENGINE_M2MF;
   if (engines & NVKMD_ENGINE_COMPUTE)
      rm_engines |= NVRM_ENGINE_COMPUTE;

   struct nvkmd_darwin_exec_ctx *ctx =
      CALLOC_STRUCT(nvkmd_darwin_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_darwin_exec_ctx_ops;
   ctx->base.dev = dev;
   util_dynarray_init(&ctx->waits, NULL);
   util_dynarray_init(&ctx->signals, NULL);
   util_dynarray_init(&ctx->pushes, NULL);
   util_dynarray_init(&ctx->gpfifo, NULL);

   result = nvkmd_dev_alloc_mapped_mem(dev, log_obj, NVKMD_DARWIN_RING_B, 0,
                                       NVKMD_MEM_GART, NVKMD_MEM_MAP_WR,
                                       &ctx->ring);
   if (result != VK_SUCCESS)
      goto fail_ctx;

   result = nvkmd_darwin_sem_alloc(pdev, log_obj, &ctx->fence);
   if (result != VK_SUCCESS)
      goto fail_ring;
   *ctx->fence.map = 0;

   simple_mtx_lock(&pdev->rm_mutex);
   int err = nvrm_chan_create(pdev->rm, rm_engines,
                              NVKMD_DARWIN_GPFIFO_ENTRIES, &ctx->chan);
   simple_mtx_unlock(&pdev->rm_mutex);
   if (err != 0) {
      result = vk_errorf(log_obj,
                         err == -ENOMEM ? VK_ERROR_OUT_OF_DEVICE_MEMORY :
                                          VK_ERROR_INITIALIZATION_FAILED,
                         "Failed to create a channel: %s", strerror(-err));
      goto fail_fence;
   }
   ctx->max_submit = nvrm_chan_info(ctx->chan)->entries / 2;

   *ctx_out = &ctx->base;

   return VK_SUCCESS;

fail_fence:
   nvkmd_darwin_sem_free(pdev, &ctx->fence);
fail_ring:
   nvkmd_mem_unref(ctx->ring);
fail_ctx:
   FREE(ctx);

   return result;
}

static void
nvkmd_darwin_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   UNUSED VkResult result = nvkmd_darwin_sem_wait(pdev, &ctx->fence,
                                                  ctx->seqno);

   simple_mtx_lock(&pdev->rm_mutex);
   nvrm_chan_destroy(ctx->chan);
   simple_mtx_unlock(&pdev->rm_mutex);

   nvkmd_darwin_sem_free(pdev, &ctx->fence);
   nvkmd_mem_unref(ctx->ring);
   util_dynarray_fini(&ctx->waits);
   util_dynarray_fini(&ctx->signals);
   util_dynarray_fini(&ctx->pushes);
   util_dynarray_fini(&ctx->gpfifo);
   FREE(ctx);
}

static VkResult
nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   const uint32_t wait_count =
      util_dynarray_num_elements(&ctx->waits, struct nvkmd_darwin_ctx_wait);
   const uint32_t signal_count =
      util_dynarray_num_elements(&ctx->signals, struct nvkmd_darwin_ctx_signal);
   const uint32_t push_count =
      util_dynarray_num_elements(&ctx->pushes, struct nvrm_push);

   if (wait_count == 0 && signal_count == 0 && push_count == 0)
      return VK_SUCCESS;

   util_dynarray_clear(&ctx->gpfifo);
   if (util_dynarray_ensure_cap(&ctx->gpfifo,
                                (push_count + 2) * sizeof(struct nvrm_push)) ==
       NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t pre_B = wait_count * NVKMD_DARWIN_SEM_DW * 4;
   const uint32_t post_B = (signal_count + 1) * NVKMD_DARWIN_SEM_DW * 4;
   uint32_t offset;
   VkResult result = ring_alloc(pdev, ctx, pre_B + post_B, &offset);
   if (result != VK_SUCCESS)
      return vk_error(log_obj, result);

   const uint64_t addr = ctx->ring->va->addr + offset;
   const uint64_t seqno = ctx->seqno + 1;

   struct nv_push push;
   nv_push_init(&push, (uint32_t *)((char *)ctx->ring->map + offset),
                (pre_B + post_B) / 4, SUBC_MASK_ALL);

   util_dynarray_foreach(&ctx->waits, struct nvkmd_darwin_ctx_wait, wait)
      push_sem_acquire(&push, wait->addr, wait->value);
   assert(nv_push_dw_count(&push) * 4 == pre_B);

   util_dynarray_foreach(&ctx->signals, struct nvkmd_darwin_ctx_signal, signal)
      push_sem_release(&push, signal->sync->sem.addr, signal->value);
   push_sem_release(&push, ctx->fence.addr, seqno);
   assert(nv_push_dw_count(&push) * 4 == pre_B + post_B);

   if (pre_B > 0) {
      const struct nvrm_push pre = { .addr = addr, .size = pre_B };
      util_dynarray_append(&ctx->gpfifo, pre);
   }
   if (push_count > 0)
      util_dynarray_append_array(&ctx->gpfifo, struct nvrm_push,
                                 util_dynarray_begin(&ctx->pushes),
                                 push_count);
   const struct nvrm_push post = { .addr = addr + pre_B, .size = post_B };
   util_dynarray_append(&ctx->gpfifo, post);

   const struct nvrm_push *entries = util_dynarray_begin(&ctx->gpfifo);
   const uint32_t entry_count =
      util_dynarray_num_elements(&ctx->gpfifo, struct nvrm_push);

   uint32_t iter = 0;
   for (uint32_t i = 0; i < entry_count;) {
      const uint32_t n = MIN2(entry_count - i, ctx->max_submit);

      simple_mtx_lock(&pdev->rm_mutex);
      int err = nvrm_chan_submit(ctx->chan, entries + i, n);
      simple_mtx_unlock(&pdev->rm_mutex);

      if (err == -EAGAIN) {
         result = nvkmd_darwin_backoff(pdev, &iter);
         if (result != VK_SUCCESS)
            return vk_error(log_obj, result);
         continue;
      }
      if (err != 0)
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "Failed to submit to the channel: %s",
                          strerror(-err));
      i += n;
      iter = 0;
   }

   ctx->seqno = seqno;
   ring_push_use(ctx, offset, pre_B + post_B, seqno);

   util_dynarray_foreach(&ctx->signals, struct nvkmd_darwin_ctx_signal, signal)
      nvkmd_darwin_sync_set_pending(signal->sync, signal->value);

   util_dynarray_clear(&ctx->waits);
   util_dynarray_clear(&ctx->signals);
   util_dynarray_clear(&ctx->pushes);

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t wait_count,
                           const struct vk_sync_wait *waits)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   for (uint32_t i = 0; i < wait_count; i++) {
      struct nvkmd_darwin_sync *sync = ctx_sync(pdev, waits[i].sync);
      if (sync == NULL || *sync->sem.map >= waits[i].wait_value)
         continue;

      if (util_dynarray_num_elements(&ctx->waits,
                                     struct nvkmd_darwin_ctx_wait) >=
          NVKMD_DARWIN_MAX_SYNCS) {
         VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      const struct nvkmd_darwin_ctx_wait wait = {
         .addr = sync->sem.addr,
         .value = waits[i].wait_value,
      };
      if (util_dynarray_grow(&ctx->waits, struct nvkmd_darwin_ctx_wait, 1) ==
          NULL)
         return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      util_dynarray_top(&ctx->waits, struct nvkmd_darwin_ctx_wait) = wait;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t exec_count,
                           const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   struct nvrm_push *push =
      util_dynarray_grow(&ctx->pushes, struct nvrm_push, exec_count);
   if (push == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < exec_count; i++) {
      assert((execs[i].addr % 4) == 0 && (execs[i].size_B % 4) == 0);
      assert(execs[i].size_B < (1u << 23));

      push[i] = (struct nvrm_push) {
         .addr = execs[i].addr,
         .size = execs[i].size_B,
         .flags = execs[i].no_prefetch ? NVRM_PUSH_NO_PREFETCH : 0,
      };
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_darwin_sync *sync = ctx_sync(pdev, signals[i].sync);
      if (sync == NULL)
         continue;

      if (util_dynarray_num_elements(&ctx->signals,
                                     struct nvkmd_darwin_ctx_signal) >=
          NVKMD_DARWIN_MAX_SYNCS) {
         VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      const struct nvkmd_darwin_ctx_signal signal = {
         .sync = sync,
         .value = signals[i].signal_value,
      };
      if (util_dynarray_grow(&ctx->signals, struct nvkmd_darwin_ctx_signal,
                             1) == NULL)
         return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      util_dynarray_top(&ctx->signals, struct nvkmd_darwin_ctx_signal) = signal;
   }

   return nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
}

static VkResult
nvkmd_darwin_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   result = nvkmd_darwin_sem_wait(pdev, &ctx->fence, ctx->seqno);
   if (result != VK_SUCCESS)
      return vk_errorf(log_obj, result, "GSP reported a GPU error");

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_darwin_exec_ctx_ops = {
   .destroy = nvkmd_darwin_exec_ctx_destroy,
   .wait = nvkmd_darwin_exec_ctx_wait,
   .exec = nvkmd_darwin_exec_ctx_exec,
   .signal = nvkmd_darwin_exec_ctx_signal,
   .flush = nvkmd_darwin_exec_ctx_flush,
   .sync = nvkmd_darwin_exec_ctx_sync,
};

static VkResult
nvkmd_darwin_create_bind_ctx(struct nvkmd_dev *dev,
                             struct vk_object_base *log_obj,
                             struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_darwin_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_darwin_bind_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_darwin_bind_ctx_ops;
   ctx->base.dev = dev;

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_darwin_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_darwin_bind_ctx *ctx = nvkmd_darwin_bind_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_darwin_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t wait_count,
                           const struct vk_sync_wait *waits)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);

   for (uint32_t i = 0; i < wait_count; i++) {
      struct nvkmd_darwin_sync *sync = ctx_sync(pdev, waits[i].sync);
      if (sync == NULL)
         continue;

      VkResult result = nvkmd_darwin_sem_wait(pdev, &sync->sem,
                                              waits[i].wait_value);
      if (result != VK_SUCCESS)
         return vk_errorf(log_obj, result, "GSP reported a GPU error");
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t bind_count,
                           const struct nvkmd_ctx_bind *binds)
{
   for (uint32_t i = 0; i < bind_count; i++) {
      VkResult result;
      switch (binds[i].op) {
      case NVKMD_BIND_OP_BIND:
         result = nvkmd_va_bind_mem(binds[i].va, log_obj, binds[i].va_offset_B,
                                    binds[i].mem, binds[i].mem_offset_B,
                                    binds[i].range_B);
         break;
      case NVKMD_BIND_OP_UNBIND:
         result = nvkmd_va_unbind(binds[i].va, log_obj, binds[i].va_offset_B,
                                  binds[i].range_B);
         break;
      default:
         UNREACHABLE("Invalid bind op");
      }
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_ctx->dev->pdev);

   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_darwin_sync *sync = ctx_sync(pdev, signals[i].sync);
      if (sync == NULL)
         continue;

      nvkmd_darwin_sync_set_pending(sync, signals[i].signal_value);
      *sync->sem.map = signals[i].signal_value;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_darwin_bind_ctx_ops = {
   .destroy = nvkmd_darwin_bind_ctx_destroy,
   .wait = nvkmd_darwin_bind_ctx_wait,
   .bind = nvkmd_darwin_bind_ctx_bind,
   .signal = nvkmd_darwin_bind_ctx_signal,
   .flush = nvkmd_darwin_bind_ctx_flush,
};

VkResult
nvkmd_darwin_create_ctx(struct nvkmd_dev *dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_engines engines,
                        struct nvkmd_ctx **ctx_out)
{
   if (engines == NVKMD_ENGINE_BIND)
      return nvkmd_darwin_create_bind_ctx(dev, log_obj, ctx_out);

   assert(!(engines & NVKMD_ENGINE_BIND));
   return nvkmd_darwin_create_exec_ctx(dev, log_obj, engines, ctx_out);
}
