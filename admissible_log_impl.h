/*
 * admissible_log_impl.h
 *
 *  Created on: September 5, 2014
 *      Author: aousterh
 */

#include "admissible_log.h"
#include "emulation.h"
#include "../arbiter/emu_admission_core.h"
#include "../protocol/topology.h"

#include <stdio.h>

struct emu_admission_core_statistics emu_saved_admission_core_statistics[ALGO_N_CORES];
struct emu_admission_statistics emu_saved_admission_statistics;

void print_core_admission_log_emulation(uint16_t core_index) {
	uint64_t dropped_in_algo;
	Emulation *emulation = emu_get_instance();
	struct emu_admission_core_statistics *st =
			emulation->m_core_stats[core_index];
	struct emu_admission_core_statistics *sv =
			&emu_saved_admission_core_statistics[core_index];

	printf("\nadmission core %d", core_index);
/*#define D(X) (st->X - sv->X)
	printf("\n  admitted waits: %lu, admitted alloc fails: %lu",
			D(wait_for_admitted_enqueue), D(admitted_alloc_failed));
	printf("\n  packets: %lu admitted, %lu dropped, %lu marked",
			D(admitted_packet), D(dropped_packet), D(marked_packet));
	printf("\n  endpoint driver pushed %lu, pulled %lu, new %lu",
			D(endpoint_driver_pushed), D(endpoint_driver_pulled),
			D(endpoint_driver_processed_new));
	printf("\n  router driver pushed %lu, pulled %lu", D(router_driver_pushed),
			D(router_driver_pulled));
	printf("\n");
#undef D*/

	printf("\n  admitted waits: %lu, admitted alloc fails: %lu",
			st->wait_for_admitted_enqueue, st->admitted_alloc_failed);
	printf("\n  packets: %lu admitted, %lu dropped, %lu marked",
			st->admitted_packet, st->dropped_packet, st->marked_packet);
	printf("\n  endpoint driver pushed %lu, pulled %lu, new %lu",
			st->endpoint_driver_pushed, st->endpoint_driver_pulled,
			st->endpoint_driver_processed_new);
	printf("\n  router driver pushed %lu, pulled %lu",
			st->router_driver_pushed, st->router_driver_pulled);

	printf("\n warnings:");
	if (st->send_packet_failed)
		printf("\n  %lu send packet failed", st->send_packet_failed);
	printf("\n");
}

void print_global_admission_log_emulation() {
	uint64_t dropped_in_algo;
	Emulation *emulation = emu_get_instance();
	struct emu_admission_statistics *st = &emulation->m_stat;
	struct emu_admission_statistics *sv = &emu_saved_admission_statistics;

	printf("\nemulation with %d nodes", NUM_NODES);

#if defined(DCTCP)
	printf("\nrouter type DCTCP");
#elif defined(RED)
	printf("\nrouter type RED");
#elif defined(DROP_TAIL)
	printf("\nrouter type drop tail");
#elif defined(HULL)
	printf("\nrouter type HULL");
#endif

	printf("\n warnings:");
	if (st->packet_alloc_failed)
		printf("\n  %lu packet allocs failed (increase packet mempool size?)",
				st->packet_alloc_failed);
	if (st->enqueue_backlog_failed)
		printf("\n  %lu enqueue backlog failed", st->enqueue_backlog_failed);
	if (st->enqueue_reset_failed)
		printf("\n  %lu enqueue reset failed", st->enqueue_reset_failed);
	printf("\n");
}

void emu_save_admission_stats() {
	Emulation *emulation = emu_get_instance();
	memcpy(&emu_saved_admission_statistics, &emulation->m_stat,
			sizeof(emu_saved_admission_statistics));
}

void emu_save_admission_core_stats(int core_index) {
	uint16_t i;

	Emulation *emulation = emu_get_instance();
	memcpy(&emu_saved_admission_core_statistics[core_index],
			emulation->m_core_stats[core_index],
			sizeof(struct emu_admission_core_statistics));
}
