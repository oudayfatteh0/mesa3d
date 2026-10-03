/*
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_DARWIN_H
#define NVKMD_DARWIN_H 1

#include "nvkmd/nvkmd.h"

#include <libnvrm.h>

struct nvkmd_darwin_pdev {
   struct nvkmd_pdev base;

   struct nvrm_platform plat;
   struct nvrm_dev *rm;

   const struct vk_sync_type *sync_types[1];
};

NVKMD_DECL_SUBCLASS(pdev, darwin);

VkResult nvkmd_darwin_try_create_pdev(struct vk_object_base *log_obj,
                                      enum nvk_debug debug_flags,
                                      struct nvkmd_pdev **pdev_out);

#endif
