/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

Textual (benchmark) client for the N-body simulation.
Inspired by textual.c from Fractal @ PCAD.

Usage:
  textual <host> <port> <num_particles> <num_iterations> <space_size> [<seed>]

The client:
  1. Connects to the coordinator and discovers the worker count.
  2. Generates <num_particles> random particles inside a square
     spatial domain of side <space_size> metres.
  3. Sends a payload requesting <num_iterations> simulation steps.
  4. Receives and prints a summary of every response.
  5. Sends a shutdown signal and exits.

Thread model (mirrors fractal textual.c):
  - main thread         : orchestration, timing
  - payload_thread      : dequeue payload → send to coordinator
  - response_thread     : recv from coordinator → enqueue response
  - ui_thread           : build payload and enqueue (stub)
  - render_thread       : dequeue response and print (stub)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/types.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>

#include "ncorpos.h"
#include "galaxy_ic.h"
#include "connection.h"
#include "queue.h"
#include "timing.h"
#include "logging.h"

atomic_int shutdown_requested;

static queue_t payload_queue  = {0};
static queue_t response_queue = {0};

/* ---------------------------------------------------------------
 * Shutdown helper
 * --------------------------------------------------------------- */
static void request_shutdown(int connection)
{
  if (atomic_exchange(&shutdown_requested, 1) == 1)
    return;
  printf("Shutdown requested.\n");
  shutdown(connection, SHUT_RDWR);
  queue_enqueue(&payload_queue,  NULL);
  queue_enqueue(&response_queue, NULL);
}

/* ---------------------------------------------------------------
 * ui_thread_function (placeholder / stub)
 *
 * In a real interactive client this would read user input and
 * build new payloads.  Here it does nothing; the main thread
 * enqueues the payload directly.
 * --------------------------------------------------------------- */
static void *ui_thread_function(void *arg)
{
  (void)arg;
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * render_thread_function (placeholder / stub)
 *
 * In a real interactive client this would display the received
 * particle positions.  Here it just frees responses.
 * --------------------------------------------------------------- */
static void *render_thread_function(void *arg)
{
  /*
   * Stub: response rendering is handled by the main thread.
   * This thread must NOT dequeue from response_queue — doing so
   * would race with the main thread's response-counting loop and
   * cause it to hang waiting for responses that were silently freed.
   */
  (void)arg;
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * net_thread_send_payload
 * --------------------------------------------------------------- */
static void *net_thread_send_payload(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    payload_t *p = (payload_t *)queue_dequeue(&payload_queue);
    if (p == NULL) break; /* poison pill */

    /* Send fixed-size header */
    if (send(connection, p, sizeof(payload_t), 0) <= 0) {
      fprintf(stderr, "Send failed (payload header). Killing thread.\n");
      free_payload(p);
      pthread_exit(NULL);
    }

    /* Send particle array (if not a control payload) */
    if (p->generation != PAYLOAD_GENERATION_SHUTDOWN && p->particles) {
      size_t buf_size = (size_t)p->num_particles * sizeof(particle_t);
      if (send(connection, p->particles, buf_size, 0) <= 0) {
        fprintf(stderr, "Send failed (particles). Killing thread.\n");
        free_payload(p);
        pthread_exit(NULL);
      }
    }

    free_payload(p);
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * net_thread_receive_response
 * --------------------------------------------------------------- */
static void *net_thread_receive_response(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    response_t *r = calloc(1, sizeof(response_t));
    if (!r) { fprintf(stderr, "calloc failed\n"); pthread_exit(NULL); }

    /* Receive fixed-size header */
    if (recv_all(connection, r, sizeof(response_t), 0) <= 0) {
      fprintf(stderr, "Receive failed (response header). Killing thread.\n");
      free(r);
      pthread_exit(NULL);
    }

    /* Receive particle array */
    int slice = r->num_particles_slice;
    r->particles = malloc((size_t)slice * sizeof(particle_t));
    if (!r->particles) { fprintf(stderr, "malloc failed\n"); free(r); pthread_exit(NULL); }

    if (recv_all(connection, r->particles,
                 (size_t)slice * sizeof(particle_t), 0) <= 0) {
      fprintf(stderr, "Receive failed (particle array). Killing thread.\n");
      free_response(r);
      pthread_exit(NULL);
    }

    queue_enqueue(&response_queue, r);
    r = NULL;
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * Argument parsing and payload construction
 * --------------------------------------------------------------- */
static payload_t *build_payload(int argc, char *argv[], int num_workers)
{
  if (argc < 6) {
    fprintf(stderr,
      "Usage: %s <host> <port> <num_particles> <num_iterations> <space_size> [<seed>]\n",
      argv[0]);
    exit(1);
  }

  int    num_particles  = atoi(argv[3]);
  int    num_iterations = atoi(argv[4]);
  double space_size     = atof(argv[5]);
  long   seed           = (argc >= 7) ? atol(argv[6]) : 42L;

  if (num_particles < NCORPOS_MIN_PARTICLES)
    num_particles = NCORPOS_MIN_PARTICLES;
  if (num_particles > NCORPOS_MAX_PARTICLES)
    num_particles = NCORPOS_MAX_PARTICLES;
  if (num_iterations <= 0)
    num_iterations = 1;
  if (space_size <= 0.0)
    space_size = GALAXY_RADIUS * 2.0; /* ~40 kpc diameter */

  payload_t *p = calloc(1, sizeof(payload_t));
  if (!p) { perror("calloc"); exit(1); }

  p->generation    = 1;
  p->num_particles = num_particles;
  p->num_iterations= num_iterations;
  p->num_workers   = num_workers;

  p->particles = malloc((size_t)num_particles * sizeof(particle_t));
  if (!p->particles) { perror("malloc"); exit(1); }

  generate_galaxy_ic(p->particles, num_particles, seed);

  return p;
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
  if (argc < 6) {
    fprintf(stderr,
      "Usage: %s <host> <port> <num_particles> <num_iterations> <space_size> [<seed>]\n",
      argv[0]);
    return 1;
  }

  atomic_init(&shutdown_requested, 0);
  signal(SIGPIPE, SIG_IGN);

  queue_init(&payload_queue,  1,      free_payload);
  queue_init(&response_queue, 65536,  free_response);

#if LOG_LEVEL >= LOG_BASIC
  FILE *client_log = fopen("client_log.txt", "w");
  if (!client_log) {
    fprintf(stderr, "Failed to open client_log.txt\n");
    return 1;
  }
#endif

  int connection = open_connection(argv[1], (uint16_t)atoi(argv[2]));

  /* Handshake: read available worker count from coordinator */
  int max_workers = 1;
  if (recv_all(connection, &max_workers, sizeof(int), 0) <= 0) {
    fprintf(stderr, "Failed to receive worker count from coordinator.\n");
    return 1;
  }
  printf("Coordinator has %d workers available.\n", max_workers);

  payload_t *payload = build_payload(argc, argv, max_workers);

  /* Expected number of responses: num_workers × num_iterations */
  int expected = payload->num_workers * payload->num_iterations;
  printf("Expecting %d responses (%d workers × %d iterations).\n",
         expected, payload->num_workers, payload->num_iterations);

  pthread_t ui_thread       = 0;
  pthread_t render_thread   = 0;
  pthread_t payload_thread  = 0;
  pthread_t response_thread = 0;

  pthread_create(&ui_thread,       NULL, ui_thread_function,        NULL);
  pthread_create(&render_thread,   NULL, render_thread_function,    NULL);
  pthread_create(&payload_thread,  NULL, net_thread_send_payload,   &connection);
  pthread_create(&response_thread, NULL, net_thread_receive_response, &connection);

#if LOG_LEVEL >= LOG_BASIC
  struct timespec t_enqueue, t_first, t_end;
  clock_gettime(CLOCK_MONOTONIC, &t_enqueue);
#endif

  queue_enqueue(&payload_queue, payload);
  payload = NULL;

  /* Receive all expected responses */
  for (int i = 0; i < expected; i++) {
    response_t *r = (response_t *)queue_dequeue(&response_queue);
    if (r == NULL) break;

#if LOG_LEVEL >= LOG_BASIC
    if (i == 0)
      clock_gettime(CLOCK_MONOTONIC, &t_first);
#endif

    printf("Response gen=%d iter=%d worker=%d/%d  slice=[%d, %d)  "
           "first particle: id=%d x=%.3e y=%.3e\n",
           r->generation, r->iteration,
           r->worker_id,  r->max_worker_id,
           r->first_particle,
           r->first_particle + r->num_particles_slice,
           r->particles ? r->particles[0].id   : -1,
           r->particles ? r->particles[0].x    : 0.0,
           r->particles ? r->particles[0].y    : 0.0);

    free_response(r);
  }

#if LOG_LEVEL >= LOG_BASIC
  clock_gettime(CLOCK_MONOTONIC, &t_end);
  fprintf(client_log, "[FIRST_RESPONSE]: %.9f\n",
          timespec_to_double(timespec_diff(t_enqueue, t_first)));
  fprintf(client_log, "[ALL_RESPONSES]:  %.9f\n",
          timespec_to_double(timespec_diff(t_enqueue, t_end)));
  fclose(client_log);
#endif

  /* Close this session — do not kill the coordinator; it stays alive
   * to serve the next client.  request_shutdown() closes our socket and
   * sends poison pills to the local queues so all threads exit. */
  request_shutdown(connection);
  pthread_join(response_thread, NULL);
  pthread_join(payload_thread,  NULL);
  pthread_join(ui_thread,       NULL);
  pthread_join(render_thread,   NULL);

  close(connection);
  queue_destroy(&payload_queue);
  queue_destroy(&response_queue);

  return 0;
}
