/*
 * router.c
 *
 *  Created on: July 7, 2014
 *      Author: aousterh
 */

#include "router.h"
#include "api.h"
#include "api_impl.h"
#include "port.h"
#include "../graph-algo/fp_ring.h"
#include "../graph-algo/platform.h"
#include "assert.h"

int router_init(struct emu_router *rtr, uint16_t id, struct emu_ops *ops) {
	uint16_t i;

	assert(rtr != NULL);
	assert(ops != NULL);

	rtr->id = id;
	rtr->ops = &ops->rtr_ops;

	return rtr->ops->init(rtr, ops->args);
}

void router_cleanup(struct emu_router *rtr) {
	uint16_t i;
	struct emu_packet *packet;

	rtr->ops->cleanup(rtr);

	/* free egress queues in ports (ingress will be freed by other port) */
	for (i = 0; i < EMU_ROUTER_NUM_PORTS; i++) {
		while (fp_ring_dequeue(rtr->ports[i].q_egress, (void **) &packet) == 0) {
			free_packet(packet);
		}
		fp_free(rtr->ports[i].q_egress);
	}
}

void router_emulate(struct emu_router *rtr) {
	uint16_t i;
	struct emu_port *port;
	struct emu_packet *packet;

	/* for each output, try to fetch a packet and send it */
	for (i = 0; i < EMU_ROUTER_NUM_PORTS; i++) {
		port = router_port(rtr, i);
		rtr->ops->send(rtr, i, &packet);

		if (packet != NULL) {
			adm_log_emu_router_sent_packet(&g_state->stat);
			send_packet(port, packet);
		}
	}

	/* pass all incoming packets to the router */
	for (i = 0; i < EMU_ROUTER_NUM_PORTS; i++) {
		port = router_port(rtr, i); /* TODO: remove ports */

		if ((packet = receive_packet(port)) != NULL)
			rtr->ops->receive(rtr, packet);
	}
}
