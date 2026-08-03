#ifndef BUZZOS_VIRTIO_INPUT_H
#define BUZZOS_VIRTIO_INPUT_H

/* Initialize the modern PCI VirtIO input tablet when present.  Returns zero
 * only after an interrupt-driven absolute event queue is live. */
int virtio_input_init(void);

#endif /* BUZZOS_VIRTIO_INPUT_H */
