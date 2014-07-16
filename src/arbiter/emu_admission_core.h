/*
 * emu_admission_core.h
 *
 *  Created on: July 12, 2014
 *      Author: aousterh
 */

#ifndef EMU_ADMISSION_CORE_H
#define EMU_ADMISSION_CORE_H

#include <rte_ring.h>

#define		ADMITTED_TRAFFIC_MEMPOOL_SIZE		(16 * N_ADMISSION_CORES)
#define		ADMITTED_TRAFFIC_CACHE_SIZE		8

#define		PACKET_MEMPOOL_CACHE_SIZE		256

#define		PACKET_Q_SIZE                           (0x1 << PACKET_Q_LOG_SIZE)

/* emu state */
extern struct emu_state g_emu_state;

void emu_admission_init_global(struct rte_ring *q_admitted_out);

/**
 * Runs the admission core
 */
int exec_emu_admission_core(void *void_cmd_p);


#endif /* EMU_ADMISSION_CORE_H */