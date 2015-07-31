/*
 * emulation.cc
 *
 *  Created on: June 24, 2014
 *      Author: aousterh
 */

#include "emulation.h"
#include "admitted.h"
#include "emulation_core.h"
#include "endpoint_group.h"
#include "router.h"
#include "drivers/EndpointDriver.h"
#include "drivers/RouterDriver.h"
#include "output.h"
#include "packet_impl.h"
#include "util/make_mempool.h"
#include "util/make_ring.h"
#include "../protocol/topology.h"
#include "../graph-algo/fp_ring.h"
#include "../graph-algo/platform.h"

#include <assert.h>
#include <stdio.h>

Emulation::Emulation(struct fp_mempool *admitted_traffic_mempool,
		struct fp_ring *q_admitted_out, uint32_t packet_ring_size,
		RouterType r_type, void *r_args, EndpointType e_type, void *e_args,
		struct emu_topo_config *m_topo_config)
	: m_admitted_traffic_mempool(admitted_traffic_mempool),
	  m_q_admitted_out(q_admitted_out),
	  m_topo_config(m_topo_config) {
	uint32_t i, pq;
	EndpointDriver	*endpoint_drivers[EMU_MAX_ENDPOINT_GROUPS];
	RouterDriver	*router_drivers[EMU_MAX_ROUTERS];
	EmulationOutput *out;
	Dropper *dropper;
	char s[64];
	struct fp_ring *packet_queues[EMU_MAX_PACKET_QS];

	/* create packet mempool */
	m_packet_mempool = make_mempool("packet_mempool", PACKET_MEMPOOL_SIZE,
			EMU_ALIGN(sizeof(struct emu_packet)), PACKET_MEMPOOL_CACHE_SIZE, 0,
			0);

	/* init packet_queues */
	pq = 0;
	for (i = 0; i < num_packet_qs(m_topo_config); i++) {
		snprintf(s, sizeof(s), "packet_q_%d", i);
		packet_queues[i] = make_ring(s, packet_ring_size, 0, RING_F_SC_DEQ);
	}

	/* initialize log to zeroes */
	memset(&m_stat, 0, sizeof(struct emu_admission_statistics));

	/* initialize state used to communicate with comm cores */
	for (i = 0; i < epgs_per_comm(m_topo_config); i++) {
		m_comm_state.q_epg_new_pkts[i] = packet_queues[pq++];
		m_comm_state.q_resets[i] = packet_queues[pq++];
	}

	/* initialize the topology */
	construct_topology(&packet_queues[pq], &endpoint_drivers[0],
			&router_drivers[0], r_type, r_args, e_type, e_args);

	/* assign endpoints and routers to cores */
	assign_components_to_cores(endpoint_drivers, router_drivers);

	/* get queue bank stat pointers - must be done after components are
	 * assigned to cores */
	for (i = 0; i < num_routers(m_topo_config); i++) {
		m_queue_bank_stats[i] = router_drivers[i]->get_queue_bank_stats();
		m_port_drop_stats[i] = router_drivers[i]->get_port_drop_stats();
	}
}

void Emulation::step() {
	uint32_t i;

	for (i = 0; i < ALGO_N_CORES; i++)
		m_cores[i]->step();
}

void Emulation::cleanup() {
	uint32_t i;
	struct emu_admitted_traffic *admitted;

	/* cleanup cores */
	for (i = 0; i < ALGO_N_CORES; i++) {
		m_cores[i]->cleanup();
		delete m_cores[i];
	}

	/* free queues to comm core */
	for (i = 0; i < num_endpoint_groups(m_topo_config); i++) {
		/* free packet queues, return packets to mempool */
		free_packet_ring(m_comm_state.q_epg_new_pkts[i], m_packet_mempool);
		free_packet_ring(m_comm_state.q_resets[i], m_packet_mempool);
	}

	/* empty queue of admitted traffic, return structs to the mempool */
	while (fp_ring_dequeue(m_q_admitted_out, (void **) &admitted) == 0)
		fp_mempool_put(m_admitted_traffic_mempool, admitted);
	fp_free(m_q_admitted_out);

	fp_free(m_admitted_traffic_mempool);
	fp_free(m_packet_mempool);
}

/* configure the topology of endpoints and routers */
void Emulation::construct_topology(struct fp_ring **packet_queues,
		EndpointDriver **endpoint_drivers, RouterDriver **router_drivers,
		RouterType r_type, void *r_args, EndpointType e_type, void *e_args) {
	uint32_t pq, i, rtr_index;
	struct fp_ring *q_epg_ingress[EMU_MAX_ENDPOINT_GROUPS];
	struct fp_ring *q_router_ingress[EMU_MAX_ROUTERS];
	EndpointGroup *epg;
	Router *rtr;
	struct fp_ring *q_router_egress[EMU_MAX_OUTPUTS_PER_RTR];
	uint64_t rtr_masks[EMU_MAX_OUTPUTS_PER_RTR];

	printf("constructing topology with %d routers and %d endpoints\n",
			num_routers(m_topo_config), num_endpoints(m_topo_config));

	/* setup rings for routers and endpoints */
	pq = 0;
	for (i = 0; i < num_endpoint_groups(m_topo_config); i++)
		q_epg_ingress[i] = packet_queues[pq++];
	for (i = 0; i < num_routers(m_topo_config); i++)
		q_router_ingress[i] = packet_queues[pq++];

	/* initialize all the endpoints */
	for (i = 0; i < num_endpoint_groups(m_topo_config); i++) {
		epg = EndpointGroupFactory::NewEndpointGroup(e_type,
				i * endpoints_per_rack(m_topo_config), e_args, m_topo_config);
		assert(epg != NULL);
		endpoint_drivers[i] =
				new EndpointDriver(m_comm_state.q_epg_new_pkts[i],
						q_router_ingress[i], q_epg_ingress[i],
						m_comm_state.q_resets[i], epg, m_packet_mempool,
						endpoints_per_rack(m_topo_config));
	}

	/* initialize the ToRs */
	set_tor_port_masks(&rtr_masks[0]);
	if (tor_neighbors(m_topo_config) == 2)
		q_router_egress[1] = q_router_ingress[num_tors(m_topo_config)];

	for (rtr_index = 0; rtr_index < num_tors(m_topo_config); rtr_index++) {
		rtr = RouterFactory::NewRouter(r_type, r_args, TOR_ROUTER, rtr_index,
				m_topo_config);
		assert(rtr != NULL);

		q_router_egress[0] = q_epg_ingress[rtr_index];
		router_drivers[rtr_index] = new RouterDriver(rtr,
				q_router_ingress[rtr_index], &q_router_egress[0],
				&rtr_masks[0], tor_neighbors(m_topo_config), m_packet_mempool,
				endpoints_per_rack(m_topo_config));
	}

	/* initialize the Core */
	if (num_core_routers(m_topo_config) > 0) {
		set_core_port_masks(&rtr_masks[0]);
		rtr = RouterFactory::NewRouter(r_type, r_args, CORE_ROUTER,
				num_tors(m_topo_config), m_topo_config);
		assert(rtr != NULL);
		router_drivers[rtr_index] = new RouterDriver(rtr,
				q_router_ingress[rtr_index], &q_router_ingress[0],
				&rtr_masks[0], core_neighbors(m_topo_config), m_packet_mempool,
				endpoints_per_rack(m_topo_config));
	}
}

/* Populate rtr_masks with a mask for each set of ports that faces a different
 * neighbor, for a ToR. */
void Emulation::set_tor_port_masks(uint64_t *rtr_masks) {
	if (num_core_routers(m_topo_config) == 0) {
		/* single rack topology */
		if (num_endpoints(m_topo_config) <= 32)
			rtr_masks[0] = 0xFFFFFFFF; /* 32 ports */
		else
			rtr_masks[0] = 0xFFFFFFFFFFFFFFFF; /* 64 ports */
	} else {
		/* multiple racks - half of ports face up, half face down */
		rtr_masks[0] = 0xFFFFFFFF;
		rtr_masks[1] = 0xFFFFFFFF00000000;
	}
}

/* Populate rtr_masks with a mask for each set of ports that faces a different
 * neighbor, for a core router. */
void Emulation::set_core_port_masks(uint64_t *rtr_masks) {
	if (num_core_routers(m_topo_config) == 0) {
		throw std::runtime_error("no core router in this topology");
	}

	if (num_tors(m_topo_config) == 2) {
		/* two neighbors - half of ports face each */
		rtr_masks[0] = 0xFFFFFFFF;
		rtr_masks[1] = 0xFFFFFFFF00000000;
	} else if (num_tors(m_topo_config) == 3) {
		/* three neighbors - 16 ports for each */
		rtr_masks[0] = 0xFFFF;
		rtr_masks[1] = 0xFFFF0000;
		rtr_masks[2] = 0xFFFF00000000;
	} else {
		throw std::runtime_error("this number of ToRs is not yet supported");
	}
}

/* map drivers to cores based on number of cores available */
void Emulation::assign_components_to_cores(EndpointDriver **epg_drivers,
		RouterDriver **router_drivers) {
	uint16_t core_index = 0;
	uint16_t i;

	if (ALGO_N_CORES == (num_routers(m_topo_config) +
			num_endpoint_groups(m_topo_config))) {
		/* put 1 router or endpoint group on each core */
		for (i = 0; i < num_endpoint_groups(m_topo_config); i++) {
			m_cores[core_index] = new EmulationCore(&epg_drivers[i], NULL, 1,
					0, core_index, m_q_admitted_out,
					m_admitted_traffic_mempool, m_packet_mempool);
			core_index++;
		}
		for (i = 0; i < num_routers(m_topo_config); i++) {
			m_cores[core_index] = new EmulationCore(NULL, &router_drivers[i],
					0, 1, core_index, m_q_admitted_out,
					m_admitted_traffic_mempool, m_packet_mempool);
			core_index++;
		}
	} else if (ALGO_N_CORES == m_topo_config->num_racks + 1) {
		/* 1 epg + 1 rtr on first num_racks cores, core router on last core */
		for (i = 0; i < m_topo_config->num_racks; i++) {
			m_cores[core_index] = new EmulationCore(&epg_drivers[i],
					&router_drivers[i], 1, 1, core_index, m_q_admitted_out,
					m_admitted_traffic_mempool, m_packet_mempool);
			core_index++;
		}
		m_cores[core_index] = new EmulationCore(NULL,
				&router_drivers[core_index], 0, 1, core_index,
				m_q_admitted_out, m_admitted_traffic_mempool,
				m_packet_mempool);
	} else if (ALGO_N_CORES == 1) {
		/* assign everything to one core */
		m_cores[core_index] = new EmulationCore(epg_drivers, router_drivers,
				num_endpoint_groups(m_topo_config), num_routers(m_topo_config),
				core_index, m_q_admitted_out, m_admitted_traffic_mempool,
				m_packet_mempool);
	} else {
		throw std::runtime_error("no specified way to assign this topology to cores");
	}

	/* Set stats pointers */
	for (i = 0; i < ALGO_N_CORES; i++)
		m_core_stats[i] = m_cores[i]->stats();
}
