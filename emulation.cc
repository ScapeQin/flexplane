/*
 * emulation.cc
 *
 *  Created on: June 24, 2014
 *      Author: aousterh
 */

#include "emulation.h"
#include "api.h"
#include "api_impl.h"
#include "admitted.h"
#include "endpoint_group.h"
#include "router.h"
#include "../protocol/topology.h"

#include <assert.h>

#define ENDPOINT_MAX_BURST	(EMU_NUM_ENDPOINTS * 2)
#define ROUTER_MAX_BURST	(EMU_ROUTER_NUM_PORTS * 2)

emu_state *g_state; /* global emulation state */

static inline void free_packet_ring(struct fp_ring *packet_ring);

void emu_init_state(struct emu_state *state,
		struct fp_mempool *admitted_traffic_mempool,
		struct fp_ring *q_admitted_out, struct fp_mempool *packet_mempool,
	    struct fp_ring **packet_queues, RouterType r_type, void *r_args,
		EndpointType e_type, void *e_args) {
	uint32_t i, pq;
	uint32_t size;

	g_state = state;

	pq = 0;
	state->admitted_traffic_mempool = admitted_traffic_mempool;
	state->q_admitted_out = q_admitted_out;
	state->packet_mempool = packet_mempool;

	/* construct topology: 1 router with 1 rack of endpoints */

	/* initialize rings */
	for (i = 0; i < EMU_NUM_ROUTERS; i++) {
		state->q_router_ingress[i] = packet_queues[pq++];
	}
	state->q_epg_new_pkts[0] = packet_queues[pq++];
	state->q_epg_ingress[0] = packet_queues[pq++];

	/* initialize all the routers */
	for (i = 0; i < EMU_NUM_ROUTERS; i++) {
		// TODO: use fp_malloc?
		state->routers[i] = RouterFactory::NewRouter(r_type, r_args, i);
		assert(state->routers[i] != NULL);
		state->router_drivers[i] = new RouterDriver(state->routers[i],
				state->q_router_ingress[0], state->q_epg_ingress[0],
				&state->stat);
	}

	/* initialize all the endpoints in one endpoint group */
	state->endpoint_groups[0] = EndpointGroupFactory::NewEndpointGroup(e_type,
			EMU_NUM_ENDPOINTS);
	state->endpoint_groups[0]->init(0, (struct drop_tail_args *) e_args);
	assert(state->endpoint_groups[0] != NULL);
	state->endpoint_drivers[0] =
			new EndpointDriver(state->q_epg_new_pkts[0],
					state->q_router_ingress[0],
					state->q_epg_ingress[0],
					state->endpoint_groups[0],
					&state->stat);

	/* get 1 admitted traffic for the core, init it */
	while (fp_mempool_get(state->admitted_traffic_mempool,
			(void **) &state->admitted) == -ENOENT)
		adm_log_emu_admitted_alloc_failed(&state->stat);
	admitted_init(state->admitted);
}

void emu_cleanup(struct emu_state *state) {
	uint32_t i;
	struct emu_admitted_traffic *admitted;

	/* free all endpoints */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++) {
		delete state->endpoint_drivers[i];
		delete state->endpoint_groups[i];

		/* free packet queues, return packets to mempool */
		free_packet_ring(state->q_epg_new_pkts[i]);
		free_packet_ring(state->q_epg_ingress[i]);
	}

	/* free all routers */
	for (i = 0; i < EMU_NUM_ROUTERS; i++) {
		delete state->router_drivers[i];
		// TODO: call fp_free?
		delete state->routers[i];

		/* free ingress queue for this router, return packets to mempool */
		free_packet_ring(state->q_router_ingress[i]);
	}

	/* return admitted struct to mempool */
	if (state->admitted != NULL)
		fp_mempool_put(state->admitted_traffic_mempool, state->admitted);

	/* empty queue of admitted traffic, return structs to the mempool */
	while (fp_ring_dequeue(state->q_admitted_out, (void **) &admitted) == 0)
		fp_mempool_put(state->admitted_traffic_mempool, admitted);
	fp_free(state->q_admitted_out);

	fp_free(state->admitted_traffic_mempool);
	fp_free(state->packet_mempool);
}

EndpointDriver::EndpointDriver(struct fp_ring* q_new_packets,
		struct fp_ring* q_to_router, struct fp_ring* q_from_router,
		EndpointGroup* epg, struct emu_admission_statistics *stat)
	: m_q_new_packets(q_new_packets),
	  m_q_to_router(q_to_router),
	  m_q_from_router(q_from_router),
	  m_epg(epg),
	  m_stat(stat)
{}

void EndpointDriver::step() {
	push();
	pull();
	process_new();
}

/**
 * Emulate push at a single endpoint group with index @index
 */

inline void EndpointDriver::push() {
	uint32_t n_pkts;
	struct emu_packet *pkts[ENDPOINT_MAX_BURST];

	/* dequeue packets from network, pass to endpoint group */
	n_pkts = fp_ring_dequeue_burst(m_q_from_router,
			(void **) &pkts[0], ENDPOINT_MAX_BURST);
	m_epg->push_batch(&pkts[0], n_pkts);
}

/**
 * Emulate pull at a single endpoint group with index @index
 */
inline void EndpointDriver::pull() {
	uint32_t n_pkts, i;
	struct emu_packet *pkts[MAX_ENDPOINTS_PER_GROUP];

	/* pull a batch of packets from the epg, enqueue to router */
	n_pkts = m_epg->pull_batch(&pkts[0], MAX_ENDPOINTS_PER_GROUP);
	assert(n_pkts <= MAX_ENDPOINTS_PER_GROUP);
	if (fp_ring_enqueue_bulk(m_q_to_router,
			(void **) &pkts[0], n_pkts) == -ENOBUFS) {
		/* enqueue failed, drop packets and log failure */
		for (i = 0; i < n_pkts; i++)
			drop_packet(pkts[i]);
		adm_log_emu_send_packets_failed(m_stat, n_pkts);
	} else {
		adm_log_emu_endpoint_sent_packets(m_stat, n_pkts);
	}
}

/**
 * Emulate new packets at a single endpoint group with index @index
 */
inline void EndpointDriver::process_new()
{
	uint32_t n_pkts;
	struct emu_packet *pkts[ENDPOINT_MAX_BURST];

	/* dequeue new packets, pass to endpoint group */
	n_pkts = fp_ring_dequeue_burst(m_q_new_packets,
			(void **) &pkts, ENDPOINT_MAX_BURST);
	m_epg->new_packets(&pkts[0], n_pkts);
}

RouterDriver::RouterDriver(Router* router, struct fp_ring* q_to_router,
		struct fp_ring* q_from_router, struct emu_admission_statistics* stat)
	: m_router(router),
	  m_q_to_router(q_to_router),
	  m_q_from_router(q_from_router),
	  m_stat(stat)
{}

/**
 * Emulate a timeslot at a single router with index @index
 */
inline void RouterDriver::step() {
	uint32_t i, j, n_pkts;
	struct emu_packet *pkt_ptrs[ROUTER_MAX_BURST];
	assert(ROUTER_MAX_BURST >= EMU_ROUTER_NUM_PORTS);

	/* fetch packets to send from router to endpoints */
#ifdef EMU_NO_BATCH_CALLS
	n_pkts = 0;
	for (uint32_t i = 0; i < EMU_ROUTER_NUM_PORTS; i++) {
		m_router->pull(i, &pkt_ptrs[n_pkts]);

		if (pkt_ptrs[n_pkts] != NULL)
			n_pkts++;
	}
#else
	n_pkts = m_router->pull_batch(pkt_ptrs, EMU_ROUTER_NUM_PORTS);
#endif
	assert(n_pkts <= EMU_ROUTER_NUM_PORTS);
	/* send packets to endpoint groups */
	if (fp_ring_enqueue_bulk(m_q_from_router, (void **) &pkt_ptrs[0],
			n_pkts) == -ENOBUFS) {
		/* enqueue failed, drop packets and log failure */
		for (j = 0; j < n_pkts; j++)
			drop_packet(pkt_ptrs[j]);
		adm_log_emu_send_packets_failed(m_stat, n_pkts);
	} else {
		adm_log_emu_router_sent_packets(m_stat, n_pkts);
	}

	/* fetch a batch of packets from the network */
	n_pkts = fp_ring_dequeue_burst(m_q_to_router,
			(void **) &pkt_ptrs, ROUTER_MAX_BURST);
	/* pass all incoming packets to the router */
#ifdef EMU_NO_BATCH_CALLS
	for (i = 0; i < n_pkts; i++) {
		m_router->push(pkt_ptrs[i]);
	}
#else
	m_router->push_batch(&pkt_ptrs[0], n_pkts);
#endif
}

void emu_emulate(struct emu_state *state) {
	uint32_t i;

	/* push/pull at endpoints and routers must be done in a specific order to
	 * ensure that packets pushed in one timeslot cannot be pulled until the
	 * next. */

	/* push new packets from the network to endpoints */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++)
		state->endpoint_drivers[i]->step();

	/* emulate one timeslot at each router (push and pull) */
	for (i = 0; i < EMU_NUM_ROUTERS; i++)
		state->router_drivers[i]->step();

	/* send out the admitted traffic */
	while (fp_ring_enqueue(state->q_admitted_out, state->admitted) != 0)
		adm_log_emu_wait_for_admitted_enqueue(&state->stat);

	/* get 1 new admitted traffic for the core, init it */
	while (fp_mempool_get(state->admitted_traffic_mempool,
				(void **) &state->admitted) == -ENOENT)
		adm_log_emu_admitted_alloc_failed(&state->stat);
	admitted_init(state->admitted);
}

void emu_reset_sender(struct emu_state *state, uint16_t src) {

	/* TODO: clear the packets in the routers too? */
	state->endpoint_groups[0]->reset(src);
}

/* frees all the packets in an fp_ring, and frees the ring itself */
static inline void free_packet_ring(struct fp_ring *packet_ring) {
	struct emu_packet *packet;

	while (fp_ring_dequeue(packet_ring, (void **) &packet) == 0) {
		free_packet(packet);
	}
	fp_free(packet_ring);
}
