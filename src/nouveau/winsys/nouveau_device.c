#include "nouveau_device.h"

#include "nouveau_context.h"
#include "nouveau_device_info.h"

#include "drm-uapi/nouveau_drm.h"
#include "util/hash_table.h"
#include "util/u_debug.h"
#include "util/os_file.h"
#include "util/os_misc.h"

#include <fcntl.h>
#include "nvif/cl0080.h"
#include "nvif/class.h"
#include "nvif/ioctl.h"
#include <unistd.h>
#include <xf86drm.h>

#include "clc597.h"

static int
nouveau_ws_param(int fd, uint64_t param, uint64_t *value)
{
   struct drm_nouveau_getparam data = { .param = param };

   int ret = drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM, &data, sizeof(data));
   if (ret)
      return ret;

   *value = data.value;
   return 0;
}

static int
nouveau_ws_device_alloc(int fd, struct nouveau_ws_device *dev)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_new_v0 new;
      struct nv_device_v0 dev;
   } args = {
      .ioctl = {
         .object = 0,
         .owner = NVIF_IOCTL_V0_OWNER_ANY,
         .route = 0x00,
         .type = NVIF_IOCTL_V0_NEW,
         .version = 0,
      },
      .new = {
         .handle = 0,
         .object = (uintptr_t)dev,
         .oclass = NV_DEVICE,
         .route = NVIF_IOCTL_V0_ROUTE_NVIF,
         .token = (uintptr_t)dev,
         .version = 0,
      },
      .dev = {
         .device = ~0ULL,
      },
   };

   return drmCommandWrite(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
}

static int
nouveau_ws_device_info(int fd, struct nouveau_ws_device *dev)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_mthd_v0 mthd;
      struct nv_device_info_v0 info;
   } args = {
      .ioctl = {
         .object = (uintptr_t)dev,
         .owner = NVIF_IOCTL_V0_OWNER_ANY,
         .route = 0x00,
         .type = NVIF_IOCTL_V0_MTHD,
         .version = 0,
      },
      .mthd = {
         .method = NV_DEVICE_V0_INFO,
         .version = 0,
      },
      .info = {
         .version = 0,
      },
   };

   int ret = drmCommandWriteRead(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
   if (ret)
      return ret;

   dev->info.chipset = args.info.chipset;
   dev->info.vram_size_B = args.info.ram_user;

   switch (args.info.platform) {
   case NV_DEVICE_INFO_V0_IGP:
      dev->info.type = NV_DEVICE_TYPE_IGP;
      break;
   case NV_DEVICE_INFO_V0_SOC:
      dev->info.type = NV_DEVICE_TYPE_SOC;
      break;
   case NV_DEVICE_INFO_V0_PCI:
   case NV_DEVICE_INFO_V0_AGP:
   case NV_DEVICE_INFO_V0_PCIE:
   default:
      dev->info.type = NV_DEVICE_TYPE_DIS;
      break;
   }

   STATIC_ASSERT(sizeof(dev->info.device_name) >= sizeof(args.info.name));
   memcpy(dev->info.device_name, args.info.name, sizeof(args.info.name));

   STATIC_ASSERT(sizeof(dev->info.chipset_name) >= sizeof(args.info.chip));
   memcpy(dev->info.chipset_name, args.info.chip, sizeof(args.info.chip));

   return 0;
}

static int
nouveau_ws_device_zcull_info(int fd, struct nouveau_ws_device *dev)
{
   struct drm_nouveau_get_zcull_info info;
   int ret = drmCommandRead(fd, DRM_NOUVEAU_GET_ZCULL_INFO,
                            &info, sizeof(info));

   if (!ret) {
      STATIC_ASSERT(sizeof(dev->info.zcull_info) == sizeof(info));
      memcpy(&dev->info.zcull_info, &info, sizeof(dev->info.zcull_info));

      dev->info.has_zcull_info = true;
   } else {
      dev->info.has_zcull_info = false;
   }
   return ret;
}

struct nouveau_ws_device *
nouveau_ws_device_new(drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   struct nouveau_ws_device *device = CALLOC_STRUCT(nouveau_ws_device);
   uint64_t value = 0;
   drmVersionPtr ver = NULL;

   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      goto out_open;

   ver = drmGetVersion(fd);
   if (!ver)
      goto out_err;

   if (strncmp("nouveau", ver->name, ver->name_len) != 0) {
      fprintf(stderr,
              "DRM kernel driver '%.*s' in use. NVK requires nouveau.\n",
              ver->name_len, ver->name);
      goto out_err;
   }

   uint32_t version =
      ver->version_major << 24 |
      ver->version_minor << 8  |
      ver->version_patchlevel;
   drmFreeVersion(ver);
   ver = NULL;

   if (version < 0x01000301)
      goto out_err;

   device->nouveau_version = version;

   const uint64_t KERN = NOUVEAU_WS_DEVICE_KERNEL_RESERVATION_START;
   const uint64_t TOP = 1ull << 40;
   struct drm_nouveau_vm_init vminit = { KERN, TOP-KERN };
   int ret = drmCommandWrite(fd, DRM_NOUVEAU_VM_INIT, &vminit, sizeof(vminit));
   if (ret == 0)
      device->has_vm_bind = true;

   if (nouveau_ws_device_alloc(fd, device))
      goto out_err;

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_PCI_DEVICE, &value))
      goto out_err;

   device->info.device_id = value;

   if (nouveau_ws_device_info(fd, device))
      goto out_err;

   nouveau_ws_device_zcull_info(fd, device);  /* This is allowed to fail */

   const char *name;
   if (drm_device->bustype == DRM_BUS_PCI) {
      assert(device->info.type != NV_DEVICE_TYPE_SOC);
      assert(device->info.device_id == drm_device->deviceinfo.pci->device_id);

      device->info.pci.domain       = drm_device->businfo.pci->domain;
      device->info.pci.bus          = drm_device->businfo.pci->bus;
      device->info.pci.dev          = drm_device->businfo.pci->dev;
      device->info.pci.func         = drm_device->businfo.pci->func;
      device->info.pci.revision_id  = drm_device->deviceinfo.pci->revision_id;

      name = name_for_chip(drm_device->deviceinfo.pci->device_id,
                           drm_device->deviceinfo.pci->subdevice_id,
                           drm_device->deviceinfo.pci->subvendor_id);
   } else {
      name = name_for_chip(device->info.device_id, 0, 0);
   }

   if (name != NULL) {
      size_t end = sizeof(device->info.device_name) - 1;
      strncpy(device->info.device_name, name, end);
      device->info.device_name[end] = 0;
   }

   device->fd = fd;

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_EXEC_PUSH_MAX, &value))
      device->max_push = NOUVEAU_GEM_MAX_PUSH;
   else
      device->max_push = value;

   if (drm_device->bustype == DRM_BUS_PCI &&
       !nouveau_ws_param(fd, NOUVEAU_GETPARAM_VRAM_BAR_SIZE, &value))
      device->info.bar_size_B = value;

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_GRAPH_UNITS, &value))
      goto out_err;

   device->info.gpc_count = (value >> 0) & 0x000000ff;
   device->info.tpc_count = (value >> 8) & 0x0000ffff;

   struct nouveau_ws_context *tmp_ctx;
   if (nouveau_ws_context_create(device, NOUVEAU_WS_ALL_3D_ENGINES, &tmp_ctx))
      goto out_err;

   device->info.sm = sm_for_chipset(device->info.chipset);
   device->info.cls_copy = tmp_ctx->copy.cls;
   device->info.cls_eng2d = tmp_ctx->eng2d.cls;
   device->info.cls_eng3d = tmp_ctx->eng3d.cls;
   device->info.cls_m2mf = tmp_ctx->m2mf.cls;
   device->info.cls_compute = tmp_ctx->compute.cls;

   nouveau_ws_context_destroy(tmp_ctx);

   if (!nouveau_ws_context_create(device, NOUVEAU_WS_ENGINE_VDEC, &tmp_ctx)) {
      device->info.cls_vdec = tmp_ctx->vdec.cls;
      nouveau_ws_context_destroy(tmp_ctx);
   }

   // for now we hardcode those values, but in the future Nouveau could provide that information to
   // us instead.
   device->info.max_warps_per_mp = max_warps_per_mp_for_sm(device->info.sm);
   device->info.max_blocks_per_mp = max_blocks_per_mp_for_sm(device->info.sm);
   device->info.mp_per_tpc = mp_per_tpc_for_chipset(device->info.chipset);

   /* Transfer queues require two kernel fixes:
    *   0ef5c4e4db ("nouveau: fix disabling the nonstall irq due to storm code")
    *   2cb66ae604 ("nouveau: Membar before between semaphore writes and the interrupt")
    *
    * Any kernel with compression support should have these fixes and gsp
    * by default. Turing may not be fixed without gsp, but that should be rare on
    * gsp-by-default kernels. Note that older architectures have not been fixed,
    * so they require additional kernel work before we can expose this.
    */
   device->info.has_transfer_queue = device->nouveau_version >= 0x01000401 &&
                                     device->info.cls_eng3d >= TURING_A;

   /* Video decode runs on its own NVDEC channel. nouveau only allows
    * allocating those from 1.4.3 ("VDEC contexts can be created") on, and the
    * chip has to have the engine in the first place: cls_vdec is left at zero
    * above when no VDEC context can be created.
    */
   device->info.has_video = device->nouveau_version >= 0x01000403 &&
                            device->info.cls_vdec != 0;

   init_shared_mem_sizes(&device->info);

   simple_mtx_init(&device->bos_lock, mtx_plain);
   device->bos = _mesa_pointer_hash_table_create(NULL);

   return device;

out_err:
   if (ver)
      drmFreeVersion(ver);
out_open:
   FREE(device);
   close(fd);
   return NULL;
}

void
nouveau_ws_device_destroy(struct nouveau_ws_device *device)
{
   if (!device)
      return;

   _mesa_hash_table_destroy(device->bos, NULL);
   simple_mtx_destroy(&device->bos_lock);

   close(device->fd);
   FREE(device);
}

uint64_t
nouveau_ws_device_vram_used(struct nouveau_ws_device *device)
{
   /* On Tegra, we don't have VRAM */
   if (device->info.type == NV_DEVICE_TYPE_SOC)
      return 0;

   uint64_t used = 0;
   if (nouveau_ws_param(device->fd, NOUVEAU_GETPARAM_VRAM_USED, &used))
      return 0;

   /* Zero memory used would be very strange given that it includes kernel
    * internal allocations.
    */
   assert(used > 0);

   return used;
}

uint64_t
nouveau_ws_device_timestamp(struct nouveau_ws_device *device)
{
   uint64_t timestamp = 0;
   if (nouveau_ws_param(device->fd, NOUVEAU_GETPARAM_PTIMER_TIME, &timestamp))
      return 0;

   return timestamp;
}

bool
nouveau_ws_device_has_tiled_bo(struct nouveau_ws_device *device)
{
   uint64_t has = 0;
   if (nouveau_ws_param(device->fd, NOUVEAU_GETPARAM_HAS_VMA_TILEMODE, &has))
      return false;

   return has != 0;
}
