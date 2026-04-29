/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

MPI coordinator and worker processes for the N-body simulation.
Inspired by coordinator.c from Fractal @ PCAD.

Plan #3 Architecture
====================

All MPI ranks (0..W) participate equally in computation.  The
coordinator (rank 0) both handles TCP networking AND computes its own
particle slice.  MPI_COMM_WORLD is used directly for collective
operations — no dynamic sub-communicator is needed.

Coordinator (rank 0) – 4 long-lived threads plus 2 per-connection:
  compute_dispatch_thread     : newest_payload -> WAKEUP all workers
                                -> send each worker its subpayload
                                -> run own compute loop (ncorpos_step
                                   + direct response enqueue +
                                   MPI_Allgatherv on MPI_COMM_WORLD)
  mpi_recv_responses_thread   : MPI_Probe -> receive packed response
                                from workers -> enqueue to
                                response_queue; signal workers_done
                                when job completes.
  net_thread_receive_payload  : TCP -> newest_payload   (per connection)
  net_thread_send_response    : response_queue -> TCP   (per connection)

Workers (ranks 1..W) – single-threaded idle loop:
  1. Block: MPI_Probe(from coordinator, any tag)
  2. If SHUTDOWN tag: send ack, exit loop
  3. If WAKEUP tag: receive one int (generation number)
  4. Receive subpayload from coordinator
  5. For each iteration:
       a. ncorpos_step()                         – compute new positions
       b. MPI_Isend response to coordinator      – non-blocking / async
       c. MPI_Allgatherv on MPI_COMM_WORLD       – sync all positions
  6. MPI_Wait last pending Isend
  7. Go to 1

Key simplifications vs Plan #1
  - All ranks always participate: no worker pool or pool management.
  - Single communicator (MPI_COMM_WORLD) for all collectives.
  - Coordinator (rank 0) computes its own slice and enqueues
    responses directly (no MPI send from rank 0 to itself).
  - WAKEUP message is a single int (generation) – no group array.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <mpi.h>
#include <sys/time.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>

#include "ncorpos.h"
#include "connection.h"
#include "queue.h"
#include "mpi_comm.h"
#include "timing.h"
#include "logging.h"

/* ---------------------------------------------------------------
 * Per-job tracking (one job at a time)
 * --------------------------------------------------------------- */
typedef struct {
  int   generation;
  int   num_workers;          /* total MPI ranks (incl. rank 0)      */
  int   responses_expected;   /* MPI responses expected (ranks 1..W) */
  int   responses_received;
  bool  active;
} job_t;

/* ---------------------------------------------------------------
 * Coordinator-side globals
 * --------------------------------------------------------------- */
static atomic_int shutdown_requested = 0;

/* Newest payload waiting to be dispatched */
static payload_t        *newest_payload       = NULL;
static atomic_int        latest_generation    = -1;
static pthread_mutex_t   newest_payload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    new_payload          = PTHREAD_COND_INITIALIZER;

/* Workers-done signal: set by mpi_recv_responses_thread when all MPI
 * responses for the current job have arrived (workers are going idle). */
static pthread_mutex_t workers_done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  workers_done_cond  = PTHREAD_COND_INITIALIZER;
static bool            workers_job_done   = true; /* no job active initially */

static int total_workers    = 0; /* world_size - 1: ranks that respond via MPI */
static int world_size_total = 0; /* world_size: all participating ranks        */

/* Current job */
static job_t           current_job;
static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Response queue (results -> TCP send thread) */
static queue_t response_queue;

#if LOG_LEVEL >= LOG_BASIC
static FILE           *coordinator_log              = NULL;
static struct timespec payload_received_time;
static struct timespec payload_discretized_time;
static struct timespec first_response_received_time;
static struct timespec last_response_received_time;
static struct timespec first_response_sent_time;
static struct timespec last_response_sent_time;
static int     expected_responses             = 0;
static int     responses_received_count       = 0;
static int     responses_sent_count           = 0;
#endif

/* ---------------------------------------------------------------
 * handle_client_disconnect  –  called on any connection termination.
 *
 * Closes the socket, invalidates latest_generation (so that
 * mpi_recv_responses_thread and compute_dispatch_thread discard
 * any in-flight responses instead of enqueuing them for a dead
 * connection), and wakes compute_dispatch_thread if it is blocked
 * waiting for a new payload.
 *
 * Does NOT set shutdown_requested — the coordinator continues
 * running and will accept the next client after this call returns.
 * --------------------------------------------------------------- */
static void handle_client_disconnect(int connection)
{
  shutdown(connection, SHUT_RDWR);

  /* Discard any in-flight responses from the just-ended job. */
  atomic_store(&latest_generation, -1);

  /* Wake compute_dispatch_thread if blocked waiting for a payload */
  pthread_mutex_lock(&newest_payload_mutex);
  if (newest_payload != NULL) {
    free_payload(newest_payload);
    newest_payload = NULL;
  }
  pthread_cond_signal(&new_payload);
  pthread_mutex_unlock(&newest_payload_mutex);
}

/* ---------------------------------------------------------------
 * request_shutdown  –  called only when a PAYLOAD_GENERATION_SHUTDOWN
 *                      payload is received.  Sets the permanent
 *                      shutdown flag so all long-lived threads exit.
 * --------------------------------------------------------------- */
static void request_shutdown(int connection)
{
  if (atomic_exchange(&shutdown_requested, 1) == 1)
    return;

  printf("Shutdown requested.\n");
  handle_client_disconnect(connection);

  /* Also wake compute_dispatch_thread if blocked on workers_done */
  pthread_mutex_lock(&workers_done_mutex);
  pthread_cond_signal(&workers_done_cond);
  pthread_mutex_unlock(&workers_done_mutex);
}

/* ---------------------------------------------------------------
 * net_thread_receive_payload  (per-connection thread, rank 0)
 *
 * Reads payload_t headers + particle arrays from the TCP client.
 * Stores the most recent payload for the dispatch thread.
 * --------------------------------------------------------------- */
static void *net_thread_receive_payload(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    payload_t *p = calloc(1, sizeof(payload_t));
    if (!p) { fprintf(stderr, "calloc failed\n"); pthread_exit(NULL); }
    if (recv_all(connection, p, sizeof(payload_t), 0) <= 0) {
      free(p);
      handle_client_disconnect(connection);
      pthread_exit(NULL);
    }

    if (p->generation == PAYLOAD_GENERATION_SHUTDOWN) {
      free(p);
      request_shutdown(connection);
      break;
    }
    p->particles = malloc((size_t)p->num_particles * sizeof(particle_t));
    if (!p->particles) { fprintf(stderr, "malloc failed\n"); pthread_exit(NULL); }

    if (recv_all(connection, p->particles,
                 (size_t)p->num_particles * sizeof(particle_t), 0) <= 0) {
      free_payload(p);
      handle_client_disconnect(connection);
      pthread_exit(NULL);
    }

#if LOG_LEVEL >= LOG_BASIC
    clock_gettime(CLOCK_MONOTONIC, &payload_received_time);
#endif

    pthread_mutex_lock(&newest_payload_mutex);
    if (newest_payload != NULL)
      free_payload(newest_payload);
    newest_payload = p;
    atomic_store(&latest_generation, p->generation);
    pthread_cond_signal(&new_payload);
    pthread_mutex_unlock(&newest_payload_mutex);
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * compute_dispatch_thread  (long-lived, rank 0)
 *
 * Waits for a new payload, wakes ALL workers, then runs the compute
 * loop for rank 0's own particle slice.  Rank 0 participates in
 * MPI_Allgatherv on MPI_COMM_WORLD each iteration to synchronise
 * positions with all workers.  Responses from rank 0 are enqueued
 * directly to response_queue (no MPI send needed).
 * --------------------------------------------------------------- */
static void *compute_dispatch_thread(void *arg)
{
  (void)arg;

  while (!atomic_load(&shutdown_requested)) {
    /* 1. Wait for a new payload from the network thread */
    pthread_mutex_lock(&newest_payload_mutex);
    while (newest_payload == NULL && !atomic_load(&shutdown_requested))
      pthread_cond_wait(&new_payload, &newest_payload_mutex);

    if (atomic_load(&shutdown_requested)) {
      pthread_mutex_unlock(&newest_payload_mutex);
      break;
    }

    payload_t *payload = newest_payload;
    newest_payload = NULL;
    pthread_mutex_unlock(&newest_payload_mutex);

    /* 2. Wait for the previous job (if any) to fully drain from workers */
    pthread_mutex_lock(&workers_done_mutex);
    while (!workers_job_done && !atomic_load(&shutdown_requested))
      pthread_cond_wait(&workers_done_cond, &workers_done_mutex);
    workers_job_done = false; /* claim for this new job */
    pthread_mutex_unlock(&workers_done_mutex);

    if (atomic_load(&shutdown_requested)) {
      free_payload(payload);
      break;
    }

    /* 3. Discretize across ALL ranks (rank 0 takes subs[0]) */
    int needed = world_size_total;
    int length = 0;
    subpayload_t **subs = discretize_payload(payload, needed, &length);

#if LOG_LEVEL >= LOG_BASIC
    clock_gettime(CLOCK_MONOTONIC, &payload_discretized_time);
    fprintf(coordinator_log, "[DISCRETIZED]: %.9f\n",
            timespec_to_double(
              timespec_diff(payload_received_time, payload_discretized_time)));
    /* Total responses = rank 0 (direct) + workers (MPI) */
    expected_responses      = needed * payload->num_iterations;
    responses_received_count = 0;
    responses_sent_count     = 0;
#endif

    /* 4. Publish job info BEFORE sending WAKEUP so that
     *    mpi_recv_responses_thread can match incoming responses */
    pthread_mutex_lock(&job_mutex);
    current_job.generation         = payload->generation;
    current_job.num_workers        = needed;
    /* Only ranks 1..W send responses via MPI */
    current_job.responses_expected = total_workers * payload->num_iterations;
    current_job.responses_received = 0;
    current_job.active             = true;
    pthread_mutex_unlock(&job_mutex);

    /* 5. WAKEUP: send generation number to every worker */
    for (int i = 1; i <= total_workers; i++)
      MPI_Send(&payload->generation, 1, MPI_INT, i,
               NCORPOS_MPI_WAKEUP, MPI_COMM_WORLD);

    /* 6. Send each worker its subpayload (subs[0] is kept for rank 0) */
    for (int i = 1; i <= total_workers; i++)
      mpi_subpayload_send(subs[i], i);

    /* Original payload no longer needed; subpayloads are self-contained */
    free_payload(payload);
    payload = NULL;

    /* 7. Run rank 0's compute loop */
    subpayload_t *my_sub  = subs[0];
    int N         = my_sub->payload.num_particles;
    int num_iters = my_sub->payload.num_iterations;

    /*
     * Build Allgatherv displacement arrays using the same formula as
     * discretize_payload, so every rank computes identical arrays
     * without extra communication.
     */
    int base  = N / needed;
    int extra = N % needed;
    int *recvcounts = malloc((size_t)needed * sizeof(int));
    int *displs     = malloc((size_t)needed * sizeof(int));
    if (!recvcounts || !displs) { perror("malloc"); exit(1); }

    int disp = 0;
    for (int w = 0; w < needed; w++) {
      int cnt       = (base + (w < extra ? 1 : 0)) * (int)sizeof(particle_t);
      recvcounts[w] = cnt;
      displs[w]     = disp;
      disp         += cnt;
    }

#if LOG_LEVEL >= LOG_BASIC
    struct timespec iter_total = {0};
    struct timespec t_start, t_end;
#endif

    for (int iter = 0; iter < num_iters; iter++) {
#if LOG_LEVEL >= LOG_BASIC
      clock_gettime(CLOCK_MONOTONIC, &t_start);
#endif

      ncorpos_step(my_sub, 0.0, 0.0);

#if LOG_LEVEL >= LOG_BASIC
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      iter_total = timespec_add(iter_total, timespec_diff(t_start, t_end));
#if LOG_LEVEL >= LOG_FULL
      fprintf(coordinator_log, "[COORD_ITER_%d]: %.9f\n", iter,
              timespec_to_double(timespec_diff(t_start, t_end)));
#endif
#endif

      /* Rank 0 enqueues its response directly – no MPI send needed.
       * Only enqueue if the generation still matches the active connection. */
      response_t *resp = create_response_for_subpayload(my_sub, 0,
                                                        needed, iter);
      if (resp->generation == atomic_load(&latest_generation))
        queue_enqueue(&response_queue, resp);
      else
        free_response(resp);

      /*
       * Collective position exchange: all ranks (0..W) share their
       * updated slice so every rank has the full particle array for
       * the next iteration.  MPI_IN_PLACE avoids aliasing sendbuf
       * into recvbuf.
       */
      MPI_Allgatherv(
        MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
        my_sub->payload.particles,
        recvcounts, displs, MPI_BYTE,
        MPI_COMM_WORLD
      );
    }

#if LOG_LEVEL >= LOG_BASIC
    fprintf(coordinator_log,
            "[COORD_PAYLOAD_%d]: %.9f  iters=%d  slice=%d\n",
            my_sub->payload.generation,
            timespec_to_double(iter_total),
            num_iters,
            my_sub->last_particle - my_sub->first_particle);
    fflush(coordinator_log);
#endif

    free(recvcounts);
    free(displs);
    free_subpayload(my_sub);
    for (int i = 1; i <= total_workers; i++)
      free_subpayload(subs[i]);
    free(subs);
  }

  /*
   * Shutdown path: wait for any in-flight job to drain (workers must
   * finish their last iteration) before sending the shutdown signal.
   */
  pthread_mutex_lock(&workers_done_mutex);
  while (!workers_job_done && !atomic_load(&shutdown_requested))
    pthread_cond_wait(&workers_done_cond, &workers_done_mutex);
  pthread_mutex_unlock(&workers_done_mutex);

  int dummy = 0;
  for (int i = 1; i <= total_workers; i++)
    MPI_Send(&dummy, 1, MPI_INT, i,
             NCORPOS_MPI_SHUTDOWN_WORKER, MPI_COMM_WORLD);

  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * mpi_recv_responses_thread  (long-lived, rank 0)
 *
 * Receives packed responses from workers (ranks 1..W) via
 * mpi_response_receive_any().  Enqueues current-generation
 * responses to response_queue.  Signals workers_done when a job
 * completes.  Exits after receiving shutdown acks from all workers.
 * --------------------------------------------------------------- */
static void *mpi_recv_responses_thread(void *arg)
{
  (void)arg;
  int shutdown_acks = 0;

  while (shutdown_acks < total_workers) {
    response_t *r = mpi_response_receive_any();

    if (r->generation == PAYLOAD_GENERATION_SHUTDOWN) {
      free_response(r);
      shutdown_acks++;
      continue;
    }

    bool job_done = false;

    pthread_mutex_lock(&job_mutex);
    if (current_job.active && r->generation == current_job.generation) {
      if (r->generation == atomic_load(&latest_generation)) {
        queue_enqueue(&response_queue, r);
      } else {
        free_response(r);
        r = NULL;
      }

      current_job.responses_received++;

#if LOG_LEVEL >= LOG_BASIC
      responses_received_count++;
      if (responses_received_count == 1)
        clock_gettime(CLOCK_MONOTONIC, &first_response_received_time);
      else if (responses_received_count == expected_responses)
        clock_gettime(CLOCK_MONOTONIC, &last_response_received_time);
#endif

      if (current_job.responses_received == current_job.responses_expected) {
        job_done           = true;
        current_job.active = false;
      }
    } else {
      free_response(r);
    }
    pthread_mutex_unlock(&job_mutex);

    if (job_done) {
#if LOG_LEVEL >= LOG_BASIC
      fprintf(coordinator_log, "[MPI_RECV_FIRST]: %.9f\n",
              timespec_to_double(
                timespec_diff(payload_received_time,
                              first_response_received_time)));
      fprintf(coordinator_log, "[MPI_RECV_ALL]: %.9f\n",
              timespec_to_double(
                timespec_diff(payload_received_time,
                              last_response_received_time)));
#endif

      /* Signal compute_dispatch_thread that workers are done */
      pthread_mutex_lock(&workers_done_mutex);
      workers_job_done = true;
      pthread_cond_signal(&workers_done_cond);
      pthread_mutex_unlock(&workers_done_mutex);
    }
  }

  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * net_thread_send_response  (per-connection thread, rank 0)
 *
 * Dequeues response_t objects and sends them over TCP.
 * Exits on NULL (poison pill).
 * --------------------------------------------------------------- */
static void *net_thread_send_response(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    response_t *r = (response_t *)queue_dequeue(&response_queue);
    if (r == NULL) break;

    if (send(connection, r, sizeof(response_t), 0) <= 0) {
      free_response(r);
      /* drain so new connection starts with an empty queue */
      { void *s; while ((s = queue_try_dequeue(&response_queue)) != NULL) free_response(s); }
      pthread_exit(NULL);
    }

    size_t psize = (size_t)r->num_particles_slice * sizeof(particle_t);
    if (send(connection, r->particles, psize, 0) <= 0) {
      free_response(r);
      { void *s; while ((s = queue_try_dequeue(&response_queue)) != NULL) free_response(s); }
      pthread_exit(NULL);
    }

#if LOG_LEVEL >= LOG_BASIC
    responses_sent_count++;
    if (responses_sent_count == 1) {
      clock_gettime(CLOCK_MONOTONIC, &first_response_sent_time);
      fprintf(coordinator_log, "[NET_SEND_FIRST]: %.9f\n",
              timespec_to_double(
                timespec_diff(payload_received_time,
                              first_response_sent_time)));
    } else if (responses_sent_count == expected_responses) {
      clock_gettime(CLOCK_MONOTONIC, &last_response_sent_time);
      fprintf(coordinator_log, "[NET_SEND_ALL]: %.9f\n",
              timespec_to_double(
                timespec_diff(payload_received_time,
                              last_response_sent_time)));
      fflush(coordinator_log);
    }
#endif

    free_response(r);
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * main_coordinator  –  rank 0 entry point
 * --------------------------------------------------------------- */
static int main_coordinator(int argc, char *argv[])
{
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return 1;
  }

  world_size_total = size;      /* all ranks, including rank 0 */
  total_workers    = size - 1;  /* ranks that send responses via MPI */

  if (total_workers <= 0) {
    fprintf(stderr, "Need at least 1 MPI worker process.\n");
    return 1;
  }

  memset(&current_job, 0, sizeof(current_job));
  current_job.active = false;

  signal(SIGPIPE, SIG_IGN);

#if LOG_LEVEL >= LOG_BASIC
  coordinator_log = fopen("coordinator_log.txt", "w");
  if (!coordinator_log) {
    fprintf(stderr, "Failed to open coordinator log.\n");
    exit(1);
  }
#endif

  queue_init(&response_queue, 65536, free_response);

  /* Sync with workers */
  MPI_Barrier(MPI_COMM_WORLD);
  printf("%s: Coordinator (rank %d), %d workers ready.\n",
         argv[0], rank, total_workers);

  pthread_t compute_thr, mpi_recv_thr;
  pthread_create(&compute_thr,  NULL, compute_dispatch_thread,   NULL);
  pthread_create(&mpi_recv_thr, NULL, mpi_recv_responses_thread, NULL);

  int server_socket = open_server_socket((uint16_t)atoi(argv[1]));

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  while (!atomic_load(&shutdown_requested)) {
    int connection = accept(server_socket,
                            (struct sockaddr *)&client_addr, &client_len);
    if (connection < 0) break;
    printf("Accepted client connection.\n");

    /* Handshake: tell client the total number of participating ranks
     * (all ranks including rank 0 now compute) */
    if (send(connection, &world_size_total, sizeof(int), 0) <= 0) {
      fprintf(stderr, "Failed to send worker count.\n");
      close(connection);
      continue;
    }

    atomic_store(&latest_generation, -1);

    pthread_t payload_recv_thr, response_send_thr;
    pthread_create(&payload_recv_thr,  NULL, net_thread_receive_payload, &connection);
    pthread_create(&response_send_thr, NULL, net_thread_send_response,   &connection);

    pthread_join(payload_recv_thr, NULL);

    /* Signal send thread to drain and exit for this connection */
    queue_enqueue(&response_queue, NULL);
    pthread_join(response_send_thr, NULL);

    /* Clear any stale items: rank-0 responses enqueued after disconnect,
     * or a sentinel NULL from an early-exit of the send thread. */
    queue_clear(&response_queue);

    printf("Client disconnected. Waiting for new connection...\n");
    close(connection);
  }

  pthread_join(compute_thr,  NULL);
  pthread_join(mpi_recv_thr, NULL);

  shutdown(server_socket, SHUT_RDWR);
  close(server_socket);
  queue_destroy(&response_queue);

#if LOG_LEVEL >= LOG_BASIC
  fclose(coordinator_log);
#endif

  return 0;
}

/* ---------------------------------------------------------------
 * main_worker  –  ranks 1..W entry point
 *
 * Simplified vs Plan #1: no per-job sub-communicator.
 * Uses MPI_COMM_WORLD for Allgatherv; WAKEUP is a single int.
 * --------------------------------------------------------------- */
static int main_worker(int argc, char *argv[])
{
  (void)argc; (void)argv;

  int rank, world_size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

#if LOG_LEVEL >= LOG_BASIC
  if (mkdir("worker_logs", 0777) == -1 && errno != EEXIST) {
    fprintf(stderr, "Failed to create worker_logs directory.\n");
    exit(1);
  }
  char log_filename[64];
  snprintf(log_filename, sizeof(log_filename),
           "worker_logs/worker_%d.txt", rank);
  FILE *worker_log = fopen(log_filename, "w");
  if (!worker_log) {
    fprintf(stderr, "Worker %d: failed to open log file.\n", rank);
    exit(1);
  }
  struct timespec total_compute = {0};
  struct timespec t_start, t_end;
#endif

  /* Sync with coordinator */
  MPI_Barrier(MPI_COMM_WORLD);

  while (1) {
    /* Idle: block until coordinator sends something */
    MPI_Status probe_status;
    MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &probe_status);

    /* Shutdown signal */
    if (probe_status.MPI_TAG == NCORPOS_MPI_SHUTDOWN_WORKER) {
      int dummy;
      MPI_Recv(&dummy, 1, MPI_INT, 0,
               NCORPOS_MPI_SHUTDOWN_WORKER, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

      response_t ack = {0};
      ack.generation = PAYLOAD_GENERATION_SHUTDOWN;
      ack.worker_id  = rank;
      mpi_response_send_sync(&ack);
      break;
    }

    /* WAKEUP: receive generation number (one int) */
    int gen;
    MPI_Recv(&gen, 1, MPI_INT, 0,
             NCORPOS_MPI_WAKEUP, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    /* Receive subpayload from coordinator */
    subpayload_t *sub = mpi_subpayload_receive(0);

    int N          = sub->payload.num_particles;
    int num_iters  = sub->payload.num_iterations;
    int first      = sub->first_particle;
    int last       = sub->last_particle;
    int slice_size = last - first;

    /*
     * Build Allgatherv displacement arrays for MPI_COMM_WORLD.
     * Uses the same formula as discretize_payload so every rank
     * computes identical arrays without extra communication.
     */
    int base  = N / world_size;
    int extra = N % world_size;
    int *recvcounts = malloc((size_t)world_size * sizeof(int));
    int *displs     = malloc((size_t)world_size * sizeof(int));
    if (!recvcounts || !displs) { perror("malloc"); exit(1); }

    int disp = 0;
    for (int w = 0; w < world_size; w++) {
      int cnt       = (base + (w < extra ? 1 : 0)) * (int)sizeof(particle_t);
      recvcounts[w] = cnt;
      displs[w]     = disp;
      disp         += cnt;
    }

#if LOG_LEVEL >= LOG_BASIC
    struct timespec iter_total = {0};
#endif

    MPI_Request pending_req = MPI_REQUEST_NULL;
    char *pending_buf       = NULL;

    for (int iter = 0; iter < num_iters; iter++) {
      /* Wait for previous async send before overwriting anything */
      if (pending_buf != NULL) {
        MPI_Wait(&pending_req, MPI_STATUS_IGNORE);
        free(pending_buf);
        pending_buf = NULL;
      }

#if LOG_LEVEL >= LOG_BASIC
      clock_gettime(CLOCK_MONOTONIC, &t_start);
#endif

      ncorpos_step(sub, 0.0, 0.0);

#if LOG_LEVEL >= LOG_BASIC
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      iter_total = timespec_add(iter_total, timespec_diff(t_start, t_end));
#if LOG_LEVEL >= LOG_FULL
      fprintf(worker_log, "[WORKER_%d_ITER_%d]: %.9f\n", rank, iter,
              timespec_to_double(timespec_diff(t_start, t_end)));
#endif
#endif

      response_t *resp = create_response_for_subpayload(sub, rank,
                                                        world_size, iter);
      mpi_response_isend(resp, &pending_req, &pending_buf);
      free_response(resp);

      /*
       * Collective position exchange across all ranks (0..W).
       * MPI_IN_PLACE avoids aliasing sendbuf into recvbuf.
       */
      MPI_Allgatherv(
        MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
        sub->payload.particles,
        recvcounts, displs, MPI_BYTE,
        MPI_COMM_WORLD
      );
    }

    /* Wait for the last async send to complete */
    if (pending_buf != NULL) {
      MPI_Wait(&pending_req, MPI_STATUS_IGNORE);
      free(pending_buf);
    }

#if LOG_LEVEL >= LOG_BASIC
    total_compute = timespec_add(total_compute, iter_total);
    fprintf(worker_log, "[WORKER_%d_PAYLOAD_%d]: %.9f  iters=%d  slice=%d\n",
            rank, sub->payload.generation,
            timespec_to_double(iter_total),
            num_iters, slice_size);
    fflush(worker_log);
#endif

    free(recvcounts);
    free(displs);
    free_subpayload(sub);
  }

#if LOG_LEVEL >= LOG_BASIC
  fprintf(worker_log, "[WORKER_%d_TOTAL]: %.9f\n",
          rank, timespec_to_double(total_compute));
  fclose(worker_log);
#endif

  return 0;
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
  int provided;
  putenv("OMPI_MCA_pml=ob1");
  putenv("OMPI_MCA_mtl=^ofi");
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  if (provided < MPI_THREAD_MULTIPLE) {
    fprintf(stderr, "MPI does not support MPI_THREAD_MULTIPLE\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0)
    main_coordinator(argc, argv);
  else
    main_worker(argc, argv);

  MPI_Finalize();
  return 0;
}

