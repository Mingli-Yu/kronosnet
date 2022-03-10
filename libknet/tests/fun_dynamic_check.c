/*
 * Copyright (C) 2021-2022 Red Hat, Inc.  All rights reserved.
 *
 * Authors: Christine Caulfield <ccaulfie@redhat.com>
 *
 * This software licensed under GPL-2.0+
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>

#include "libknet.h"

#include "internals.h"
#include "netutils.h"
#include "test-common.h"


/*
 * Keep track of how many messages got through:
 * (includes the QUIT)
 */
#define CORRECT_NUM_MSGS 10
static int msgs_recvd = 0;

#define TESTNODES 2

static pthread_mutex_t recv_mutex = PTHREAD_MUTEX_INITIALIZER;
static int quit_recv_thread = 0;

static int reply_pipe[2];

#define FAIL_ON_ERR(fn) \
	printf("FOE: %s\n", #fn);			  \
	if ((res = fn) != 0) {				  \
	  int savederrno = errno;			  \
	  pthread_mutex_lock(&recv_mutex);		  \
	  quit_recv_thread = 1;				  \
	  pthread_mutex_unlock(&recv_mutex);		  \
	  if (recv_thread_1) {				  \
		  pthread_join(recv_thread_1, (void**)&thread_err); \
		  pthread_join(recv_thread_2, (void**)&thread_err); \
	  }						  \
	  knet_handle_stop_nodes(knet_h, TESTNODES);	  \
	  stop_logthread();				  \
	  flush_logs(logfds[0], stdout);		  \
	  close_logpipes(logfds);			  \
	  close(reply_pipe[0]);				  \
	  close(reply_pipe[1]);				  \
	  if (res == -2) {				  \
		  exit(SKIP);				  \
	  } else {					  \
		  printf("*** FAIL on line %d %s failed: %s\n", __LINE__ , #fn, strerror(savederrno)); \
		  exit(FAIL);				  \
	  }						  \
	}

static int knet_send_str(knet_handle_t knet_h, char *str)
{
	return knet_send_sync(knet_h, str, strlen(str)+1, 0);
}

/*
 * lo0 is filled in with the local address on return.
 * lo1 is expected to be provided for outgoing links, it's the actual remote address to connect to.
 */
int dyn_knet_link_set_config(knet_handle_t knet_h, knet_node_id_t host_id, uint8_t link_id,
			     uint8_t transport, uint64_t flags, int family, int dynamic,
			     char *addr,
			     struct sockaddr_storage *lo0, struct sockaddr_storage *lo1)
{
	int err = 0, savederrno = 0;
	uint32_t port;
	char portstr[32];

	for (port = 1025; port < 65536; port++) {
		sprintf(portstr, "%u", port);
		memset(lo0, 0, sizeof(struct sockaddr_storage));
		if (family == AF_INET6) {
			err = knet_strtoaddr("::1", portstr, lo0, sizeof(struct sockaddr_storage));
		} else {
			err = knet_strtoaddr(addr, portstr, lo0, sizeof(struct sockaddr_storage));
		}
		if (err < 0) {
			printf("Unable to convert loopback to sockaddr: %s\n", strerror(errno));
			goto out;
		}
		errno = 0;
		if (dynamic) {
			err = knet_link_set_config(knet_h, host_id, link_id, transport, lo0, NULL, flags);
		} else {
			err = knet_link_set_config(knet_h, host_id, link_id, transport, lo0, lo1, flags);
		}
		savederrno = errno;
		if ((err < 0) && (savederrno != EADDRINUSE)) {
			if (savederrno == EPROTONOSUPPORT && transport == KNET_TRANSPORT_SCTP) {
				return -2;
			} else {
				printf("Unable to configure link: %s\n", strerror(savederrno));
				goto out;
			}
		}
		if (!err) {
			printf("Using port %u\n", port);
			goto out;
		}
	}

	if (err) {
		printf("No more ports available\n");
	}
out:
	errno = savederrno;
	return err;
}

static void *recv_messages(void *handle)
{
	knet_handle_t knet_h = (knet_handle_t)handle;
	char buf[4096];
	ssize_t len;
	static int err = 0;
	int savederrno = 0, quit = 0;

	while ((len = knet_recv(knet_h, buf, sizeof(buf), 0)) && (!quit)) {
		savederrno = errno;
		pthread_mutex_lock(&recv_mutex);
		quit = quit_recv_thread;
		pthread_mutex_unlock(&recv_mutex);
		if (quit) {
			printf(" *** recv thread was requested to exit via FOE\n");
			err = 1;
			return &err;
		}
		if (len > 0) {
			int res;

			printf("recv: (%ld) %s\n", (long)len, buf);
			msgs_recvd++;
			if (strcmp("QUIT", buf) == 0) {
				break;
			}
			if (buf[0] == '0') { /* We should not have received this! */
				printf(" *** FAIL received packet that should have been blocked\n");
				err = 1;
				return &err;
			}
			/* Tell the main thread we have received something */
			res = write(reply_pipe[1], ".", 1);
			if (res != 1) {
				printf(" *** FAIL to send response back to main thread\n");
				err = 1;
				return &err;
			}
		}
		usleep(1000);
		if (len < 0 && savederrno != EAGAIN) {
			break;
		}
	}
	printf("-- recv thread finished: %zd %d %s\n", len, errno, strerror(savederrno));
	return &err;
}

static void notify_fn(void *private_data,
		     int datafd,
		     int8_t channel,
		     uint8_t tx_rx,
		     int error,
		     int errorno)
{
	printf("NOTIFY fn called\n");
}

/* A VERY basic filter to forward between nodes */
static int dhost_filter(void *pvt_data,
			const unsigned char *outdata,
			ssize_t outdata_len,
			uint8_t tx_rx,
			knet_node_id_t this_host_id,
			knet_node_id_t src_host_id,
			int8_t *dst_channel,
			knet_node_id_t *dst_host_ids,
			size_t *dst_host_ids_entries)
{
	dst_host_ids[0] = 3 - src_host_id;
	*dst_host_ids_entries = 1;
	return 0;
}

static int wait_for_reply(int seconds)
{
	int res;
	struct pollfd pfds;
	char tmpbuf[32];

	pfds.fd = reply_pipe[0];
	pfds.events = POLLIN | POLLERR | POLLHUP;
	pfds.revents = 0;

	res = poll(&pfds, 1, seconds*1000);
	if (res == 1) {
		if (pfds.revents & POLLIN) {
			res = read(reply_pipe[0], tmpbuf, sizeof(tmpbuf));
			if (res > 0) {
				return 0;
			}
		} else {
			printf("Error on pipe poll revent = 0x%x\n", pfds.revents);
			errno = EIO;
		}
	}
	if (res == 0) {
		errno = ETIMEDOUT;
		return -1;
	}

	return -1;
}

static void test(int transport)
{
	knet_handle_t knet_h[TESTNODES+1];
	int logfds[2];
	struct sockaddr_storage lo0, lo1;
	int res;
	pthread_t recv_thread_1 = 0;
	pthread_t recv_thread_2 = 0;
	int *thread_err;
	int datafd;
	int8_t channel;
	int seconds = 190; // dynamic tests take longer than normal tests

	if (is_memcheck() || is_helgrind()) {
		printf("Test suite is running under valgrind, adjusting wait_for_host timeout\n");
		seconds = seconds * 16;
	}

	FAIL_ON_ERR(pipe(reply_pipe));

	// Initial setup gubbins
	msgs_recvd = 0;
	setup_logpipes(logfds);
	start_logthread(logfds[1], stdout);
	knet_handle_start_nodes(knet_h, TESTNODES, logfds, KNET_LOG_DEBUG);

	FAIL_ON_ERR(knet_host_add(knet_h[2], 1));
	FAIL_ON_ERR(knet_host_add(knet_h[1], 2));

	FAIL_ON_ERR(knet_handle_enable_filter(knet_h[1], NULL, dhost_filter));
	FAIL_ON_ERR(knet_handle_enable_filter(knet_h[2], NULL, dhost_filter));

	// Create the dynamic (receiving) link
	FAIL_ON_ERR(dyn_knet_link_set_config(knet_h[1], 2, 0, transport, 0, AF_INET, 1, "127.0.0.1", &lo0, NULL));

	// Connect to the dynamic link
	FAIL_ON_ERR(dyn_knet_link_set_config(knet_h[2], 1, 0, transport, 0, AF_INET, 0, "127.0.0.1", &lo1, &lo0));

	// All the rest of the setup gubbins
	FAIL_ON_ERR(knet_handle_enable_sock_notify(knet_h[1], 0, &notify_fn));
	FAIL_ON_ERR(knet_handle_enable_sock_notify(knet_h[2], 0, &notify_fn));

	channel = datafd = 0;
	FAIL_ON_ERR(knet_handle_add_datafd(knet_h[1], &datafd, &channel));
	channel = datafd = 0;
	FAIL_ON_ERR(knet_handle_add_datafd(knet_h[2], &datafd, &channel));

	FAIL_ON_ERR(knet_link_set_enable(knet_h[1], 2, 0, 1));
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 1));

	FAIL_ON_ERR(knet_handle_setfwd(knet_h[1], 1));
	FAIL_ON_ERR(knet_handle_setfwd(knet_h[2], 1));

	// Start receive threads
	FAIL_ON_ERR(pthread_create(&recv_thread_1, NULL, recv_messages, (void *)knet_h[1]));
	FAIL_ON_ERR(pthread_create(&recv_thread_2, NULL, recv_messages, (void *)knet_h[2]));

	// Let everything settle down
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[1], TESTNODES, 1, seconds, logfds[0], stdout));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[2], TESTNODES, 1, seconds, logfds[0], stdout));

	/*
	 * TESTING STARTS HERE
	 */
	FAIL_ON_ERR(knet_send_str(knet_h[2], "Testing from 127.0.0.1"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* Test sending from the 'receiving' handle */
	FAIL_ON_ERR(knet_send_str(knet_h[1], "Testing from 'receiving' handle to 127.0.0.1"));
	// Don't wait for this on, let the error (occasionally) trigger

	/* now try 127.0.0.2 */
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 0));
	FAIL_ON_ERR(knet_link_clear_config(knet_h[2], 1, 0));

	FAIL_ON_ERR(dyn_knet_link_set_config(knet_h[2], 1, 0, transport, 0, AF_INET, 0, "127.0.0.2", &lo1, &lo0));
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 1));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[1], TESTNODES, 1, seconds, logfds[0], stdout));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[2], TESTNODES, 1, seconds, logfds[0], stdout));

	FAIL_ON_ERR(knet_send_str(knet_h[2], "Testing from 127.0.0.2"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* Test sending from the 'receiving' handle */
	FAIL_ON_ERR(knet_send_str(knet_h[1], "Testing from 'receiving' handle to 127.0.0.2"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* now try 127.0.0.3 */
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 0));
	FAIL_ON_ERR(knet_link_clear_config(knet_h[2], 1, 0));

	FAIL_ON_ERR(dyn_knet_link_set_config(knet_h[2], 1, 0, transport, 0, AF_INET, 0, "127.0.0.3", &lo1, &lo0));
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 1));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[1], TESTNODES, 1, seconds, logfds[0], stdout));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[2], TESTNODES, 1, seconds, logfds[0], stdout));

	FAIL_ON_ERR(knet_send_str(knet_h[2], "Testing from 127.0.0.3"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* Test sending from the 'receiving' handle */
	FAIL_ON_ERR(knet_send_str(knet_h[1], "Testing from 'receiving' handle to 127.0.0.3"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* Now try 127.0.0.1 again */
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 0));
	FAIL_ON_ERR(knet_link_clear_config(knet_h[2], 1, 0));

	FAIL_ON_ERR(dyn_knet_link_set_config(knet_h[2], 1, 0, transport, 0, AF_INET, 0, "127.0.0.1", &lo1, &lo0));
	FAIL_ON_ERR(knet_link_set_enable(knet_h[2], 1, 0, 1));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[1], TESTNODES, 1, seconds, logfds[0], stdout));
	FAIL_ON_ERR(wait_for_nodes_state(knet_h[2], TESTNODES, 1, seconds, logfds[0], stdout));

	FAIL_ON_ERR(knet_send_str(knet_h[2], "Testing from 127.0.0.1 again"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	/* Test sending from the 'receiving' handle */
	FAIL_ON_ERR(knet_send_str(knet_h[1], "Testing from 'receiving' handle to 127.0.0.1 again"));
	FAIL_ON_ERR(wait_for_reply(seconds));

	// Finished testing, tidy up	----------------------
	FAIL_ON_ERR(knet_send_str(knet_h[2], "QUIT"));
	FAIL_ON_ERR(knet_send_str(knet_h[1], "QUIT"));

	// Check return from the receiving thread
	pthread_join(recv_thread_1, (void**)&thread_err);
	if (*thread_err) {
		printf("Thread 1 returned %d\n", *thread_err);
		exit(FAIL);
	}
	pthread_join(recv_thread_2, (void**)&thread_err);
	if (*thread_err) {
		printf("Thread 2 returned %d\n", *thread_err);
		exit(FAIL);
	}

	//  Tidy Up
	knet_handle_stop_nodes(knet_h, TESTNODES);

	stop_logthread();
	flush_logs(logfds[0], stdout);
	close_logpipes(logfds);
	close(reply_pipe[0]);
	close(reply_pipe[1]);

	/* We could receive CORRECT_NUM_MSGS or CORRECT_NUM_MSGS-1 depending
	   on whether the first one gets lost or not (which is fine) */
	if (msgs_recvd != CORRECT_NUM_MSGS && msgs_recvd != CORRECT_NUM_MSGS-1) {
		printf("*** FAIL Recv thread got %d messages, expected %d\n", msgs_recvd, CORRECT_NUM_MSGS);
		exit(FAIL);
	}
}

int main(int argc, char *argv[])
{
	printf("Testing with UDP\n");
	test(KNET_TRANSPORT_UDP);

#ifdef HAVE_NETINET_SCTP_H
	printf("Testing with SCTP\n");
	test(KNET_TRANSPORT_SCTP);
#endif

	return PASS;
}
