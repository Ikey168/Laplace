/* Laplace core: an Intel 8254x (e1000) network card (#253).
 *
 * The card QEMU emulates by default (-nic user,model=e1000): the first one on
 * PCI (8086:100E and its relatives), legacy descriptors, 32 receive buffers
 * of 2 KiB and 32 transmit descriptors, polled (no interrupts). The card is
 * outside the machine: what it receives becomes input only when the network
 * stack (core/net.h) delivers it at a step and journals it.
 */

#ifndef CORE_E1000_H
#define CORE_E1000_H

#include <stdint.h>
#include <stdbool.h>

/* Find and start the card; false when there is none. The boot log names it. */
bool e1000_init(void);
bool e1000_present(void);
void e1000_mac(uint8_t mac[6]);

/* The next received frame (without its CRC) into buf; its length, or 0. */
uint32_t e1000_receive(uint8_t* buf, uint32_t cap);
/* Queue a frame for transmission (dropped when the ring is full). */
void e1000_send(const uint8_t* frame, uint32_t len);

#endif /* CORE_E1000_H */
