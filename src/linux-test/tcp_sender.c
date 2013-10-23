/*
 * tcp_sender.c
 *
 *  Created on: September 24, 2013
 *      Author: aousterh
 */

#include "common.h"
#include "generate_packets.h"
#include "tcp_sender.h" 

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>

#define NUM_CORES 4

enum state {
  INVALID,
  CONNECTING,
  SENDING
};

struct connection {
  enum state status;
  int sock_fd;
  int return_val;
  int bytes_left;
  char *buffer;
};
 
void tcp_sender_init(struct tcp_sender *sender, struct generator *gen,
		     uint32_t id, uint64_t start_time, uint64_t duration,
		     float clock_freq)
{
  sender->gen = gen;
  sender->id = id;
  sender->start_time = start_time;
  sender->duration = duration;
  sender->clock_freq = clock_freq;
}

// Selects the IP that corresponds to the receiver id
// Note that this only works for the original 8-machine testbed topology
// Will need to be rewritten for different topologies
void choose_IP(uint32_t receiver_id, char *ip_addr) {
  int index = 0;

  int core = rand() / ((double) RAND_MAX) * NUM_CORES + 1;
  switch(core)
    {
    case (1):
      ip_addr[index++] = '1';
      break;
    case (2):
      ip_addr[index++] = '2';
      break;
    case (3):
      ip_addr[index++] = '3';
      break;
    case (4):
      ip_addr[index++] = '4';
      break;
    default:
      assert(0);
  }
  
  ip_addr[index++] = '.';
  ip_addr[index++] = '1';
  ip_addr[index++] = '.';

  switch(receiver_id >> 1)
    {
    case (0):
      ip_addr[index++] = '1';
      break;
    case (1):
      ip_addr[index++] = '2';
      break;
    case (2):
      ip_addr[index++] = '3';
      break;
    case (3):
      ip_addr[index++] = '4';
      break;
    default:
      assert(0);
  }

  ip_addr[index++] = '.';

  switch(receiver_id % 2)
    {
    case (0):
      ip_addr[index++] = '1';
      break;
    case (1):
      ip_addr[index++] = '2';
      break;
    default:
      assert(0);
  }
  
  ip_addr[index++] = '\0';
}

void *run_tcp_sender(void *arg)
{
  struct tcp_sender *sender = (struct tcp_sender *) arg;
  struct packet outgoing;
  struct gen_packet packet;
  int count = 0;
  int i;
  struct timespec ts;
  uint32_t flows_sent = 0;

  ts.tv_sec = 0;

  uint64_t start_time = sender->start_time;
  uint64_t end_time = start_time + sender->duration;
  uint64_t next_send_time = start_time;

  // Info about connections
  struct connection connections[MAX_CONNECTIONS];
  for (i = 0; i < MAX_CONNECTIONS; i++)
    connections[i].status = INVALID;

  fd_set wfds;
  FD_ZERO(&wfds);

  // Generate the first outgoing packet
  gen_next_packet(sender->gen, &packet);
  next_send_time = start_time + packet.time;

  outgoing.sender = sender->id;
  outgoing.receiver = packet.dest;
  outgoing.send_time = next_send_time;
  outgoing.size = packet.size;
  outgoing.id = count++;

  while (current_time() < start_time);

  while (current_time() < end_time)
    {
      // Set timeout for when next packet should be sent
      uint64_t time_now = current_time();
      uint64_t time_diff = 0;
      if (next_send_time > time_now)
	time_diff = (next_send_time < end_time ? next_send_time : end_time) - time_now;
      assert(time_diff / sender->clock_freq < 1000 * 1000 * 1000);
      ts.tv_nsec = time_diff / sender->clock_freq;

      // Add fds to set and compute max
      FD_ZERO(&wfds);
      int max = 0;
      for (i = 0; i < MAX_CONNECTIONS; i++) {
	if (connections[i].status == INVALID)
	  continue;

	FD_SET(connections[i].sock_fd, &wfds);
	if (connections[i].sock_fd > max)
	  max = connections[i].sock_fd;
      }

      // Wait for a socket to be ready or for it to be time to start a new connection
      int retval = pselect(max + 1, NULL, &wfds, NULL, &ts, NULL);
      if (retval < 0)
	break;

      if (current_time() >= next_send_time) {
	// Open a new connection

	// Find an index to use
	int index = -1;
	for (i = 0; i < MAX_CONNECTIONS; i++) {
	  if (connections[i].status == INVALID)
	    {
	      connections[i].status = CONNECTING;
	      index = i;
	      break;
	    }
	}
	assert(index != -1);  // Otherwise, we have too many connections

	struct sockaddr_in sock_addr;
	struct sockaddr_in src_addr;

	// Create a socket, set to non-blocking
	connections[index].sock_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(connections[index].sock_fd != -1);
	assert(fcntl(connections[index].sock_fd, F_SETFL, O_NONBLOCK) != -1);
 
	// Initialize destination address
	memset(&sock_addr, 0, sizeof(sock_addr));
	sock_addr.sin_family = AF_INET;
	sock_addr.sin_port = htons(PORT);
	// Choose the IP that corresponds to a randomly chosen core router
	char ip_addr[12];
	choose_IP(outgoing.receiver, ip_addr);
	//printf("chosen IP %s for receiver %d\n", ip_addr, outgoing.receiver);
	assert(inet_pton(AF_INET, ip_addr, &sock_addr.sin_addr) > 0);
 
	// Connect to the receiver
	connections[index].return_val = connect(connections[index].sock_fd,
						(struct sockaddr *)&sock_addr,
						sizeof(sock_addr));
	if (connections[index].return_val < 0 && errno != EINPROGRESS) {
	  assert(current_time() > end_time);
	  return;
	}

	int size_in_bytes = outgoing.size * MTU_SIZE;
	connections[index].buffer = malloc(size_in_bytes);
	bcopy((void *) &outgoing, connections[index].buffer,
	      sizeof(struct packet));

	// Generate the next outgoing packet
	gen_next_packet(sender->gen, &packet);
	next_send_time = start_time + packet.time;

	outgoing.sender = sender->id;
	outgoing.receiver = packet.dest;
	outgoing.send_time = next_send_time;
	outgoing.size = packet.size;
	outgoing.id = count++;
      }
      
      // Handle all existing connections that are ready
      for (i = 0; i < MAX_CONNECTIONS; i++)
        {
	  int result;
	  socklen_t result_len = sizeof(result);

	  if (connections[i].status == INVALID || !FD_ISSET(connections[i].sock_fd, &wfds))
	    continue;  // This fd is invalid or not ready

	  if (connections[i].status == CONNECTING) {
	    // check that connect was successful
	    assert(getsockopt(connections[i].sock_fd, SOL_SOCKET, SO_ERROR,
			      &result, &result_len) == 0);
	    assert(result == 0);

	    // send data
	    struct packet *outgoing_data = (struct packet *) connections[i].buffer;
	    connections[i].bytes_left = outgoing_data->size * MTU_SIZE;
	    connections[i].return_val = send(connections[i].sock_fd,
					     connections[i].buffer,
					     connections[i].bytes_left, 0);
	    connections[i].status = SENDING;
	    printf("sent, \t\t%d, %d, %d, %"PRIu64", %d, %"PRIu64"\n",
		   outgoing_data->sender, outgoing_data->receiver,
		   outgoing_data->size, outgoing_data->send_time,
		   outgoing_data->id, current_time());
	    flows_sent++;
	  }
	  else {
	    // check that send was succsessful
	    assert(connections[i].return_val == connections[i].bytes_left);

	    // close socket
	    (void) shutdown(connections[i].sock_fd, SHUT_RDWR);
	    close(connections[i].sock_fd);

	    // clean up state
	    free(connections[i].buffer);
	    connections[i].status = INVALID;
	  }
	}
    }

  printf("sent %d flows\n", flows_sent);

}