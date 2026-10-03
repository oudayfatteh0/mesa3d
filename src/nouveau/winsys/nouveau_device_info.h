#ifndef NOUVEAU_DEVICE_INFO
#define NOUVEAU_DEVICE_INFO 1

#include "nv_device_info.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *name_for_chip(uint32_t dev_id,
                          uint16_t subsystem_id,
                          uint16_t subsystem_vendor_id);
uint8_t sm_for_chipset(uint16_t chipset);
uint8_t max_warps_per_mp_for_sm(uint8_t sm);
uint8_t max_blocks_per_mp_for_sm(uint8_t sm);
uint8_t mp_per_tpc_for_chipset(uint16_t chipset);
void init_shared_mem_sizes(struct nv_device_info *info);

#ifdef __cplusplus
}
#endif

#endif
