#ifndef BUZZOS_SYS_SHM_H
#define BUZZOS_SYS_SHM_H

#include <stdint.h>

/* Pin a byte range of an SHM object for a DMA user such as virtio-gpu.
 * The returned page array remains valid until shm_unpin_pages(token).  The
 * caller must already own a mapping of the object; this prevents a guessed
 * token from becoming a physical-page oracle. */
int shm_pin_pages(uint32_t token, int owner, uint32_t offset, uint32_t bytes,
                  const uintptr_t **pages_out, uint32_t *page_count_out,
                  uint32_t *first_offset_out);
void shm_unpin_pages(uint32_t token);

void shm_cleanup_owner(int owner);
#endif
