/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

MPI send/receive helpers for subpayload_t and response_t.
Plan #1 revision: response uses a single packed MPI message with
MPI_Isend (non-blocking) so the worker can overlap response
transmission with the MPI_Allgatherv position exchange.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpi_comm.h"

/* Number of int fields in the response header on the wire */
#define RESPONSE_HDR_INTS 6

/* ---------------------------------------------------------------
 * Internal helpers: payload header field-by-field send/recv
 * (coordinator <-> worker via NCORPOS_MPI_PAYLOAD_DATA)
 * --------------------------------------------------------------- */
static void _send_payload_header(const payload_t *p, int target, int tag)
{
  MPI_Send(&p->generation,     1, MPI_INT,    target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->num_particles,  1, MPI_INT,    target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->num_iterations, 1, MPI_INT,    target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->num_workers,    1, MPI_INT,    target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->space_width,    1, MPI_DOUBLE, target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->space_height,   1, MPI_DOUBLE, target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->screen_width,   1, MPI_INT,    target, tag, MPI_COMM_WORLD);
  MPI_Send(&p->screen_height,  1, MPI_INT,    target, tag, MPI_COMM_WORLD);
}

static void _recv_payload_header(payload_t *p, int source, int tag)
{
  MPI_Recv(&p->generation,     1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->num_particles,  1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->num_iterations, 1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->num_workers,    1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->space_width,    1, MPI_DOUBLE, source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->space_height,   1, MPI_DOUBLE, source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->screen_width,   1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&p->screen_height,  1, MPI_INT,    source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

/* ---------------------------------------------------------------
 * subpayload send / receive  (coordinator -> worker)
 *
 * Wire format (NCORPOS_MPI_PAYLOAD_DATA):
 *   payload header fields (field by field)
 *   first_particle  (int)
 *   last_particle   (int)
 *   particle array  (num_particles x sizeof(particle_t) bytes)
 * --------------------------------------------------------------- */
void mpi_subpayload_send(subpayload_t *sub, int target)
{
  _send_payload_header(&sub->payload, target, NCORPOS_MPI_PAYLOAD_DATA);
  MPI_Send(&sub->first_particle, 1, MPI_INT, target,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD);
  MPI_Send(&sub->last_particle,  1, MPI_INT, target,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD);
  MPI_Send(sub->payload.particles,
           sub->payload.num_particles * (int)sizeof(particle_t),
           MPI_BYTE, target,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD);
}

subpayload_t *mpi_subpayload_receive(int source)
{
  subpayload_t *sub = calloc(1, sizeof(subpayload_t));
  if (!sub) { perror("calloc"); exit(1); }

  _recv_payload_header(&sub->payload, source, NCORPOS_MPI_PAYLOAD_DATA);
  MPI_Recv(&sub->first_particle, 1, MPI_INT, source,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  MPI_Recv(&sub->last_particle,  1, MPI_INT, source,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

  int N = sub->payload.num_particles;
  sub->payload.particles = malloc((size_t)N * sizeof(particle_t));
  if (!sub->payload.particles) { perror("malloc"); exit(1); }

  MPI_Recv(sub->payload.particles,
           N * (int)sizeof(particle_t),
           MPI_BYTE, source,
           NCORPOS_MPI_PAYLOAD_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  return sub;
}

/* ---------------------------------------------------------------
 * Internal: pack a response_t into a flat byte buffer.
 *
 * Layout: [int gen][int iter][int wid][int maxwid][int first][int nslice]
 *         [particle_t * nslice]
 *
 * Returns the allocated buffer and its byte length.
 * Caller owns the buffer.
 * --------------------------------------------------------------- */
static char *_pack_response(response_t *r, size_t *total_out)
{
  size_t particle_bytes = (size_t)r->num_particles_slice * sizeof(particle_t);
  size_t total          = (size_t)RESPONSE_HDR_INTS * sizeof(int) + particle_bytes;

  char *buf = malloc(total);
  if (!buf) { perror("malloc"); exit(1); }

  int *hdr = (int *)buf;
  hdr[0] = r->generation;
  hdr[1] = r->iteration;
  hdr[2] = r->worker_id;
  hdr[3] = r->max_worker_id;
  hdr[4] = r->first_particle;
  hdr[5] = r->num_particles_slice;

  if (particle_bytes > 0)
    memcpy(buf + (size_t)RESPONSE_HDR_INTS * sizeof(int),
           r->particles, particle_bytes);

  *total_out = total;
  return buf;
}

/* ---------------------------------------------------------------
 * mpi_response_isend  –  non-blocking send (worker -> coordinator)
 *
 * The caller must keep *buf_out alive until MPI_Wait(*req_out)
 * completes, then free() it.
 * --------------------------------------------------------------- */
void mpi_response_isend(response_t *r, MPI_Request *req_out, char **buf_out)
{
  size_t total;
  char *buf = _pack_response(r, &total);

  MPI_Isend(buf, (int)total, MPI_BYTE, 0 /* coordinator */,
            NCORPOS_MPI_RESPONSE_DATA, MPI_COMM_WORLD, req_out);
  *buf_out = buf;
}

/* ---------------------------------------------------------------
 * mpi_response_send_sync  –  blocking send (worker -> coordinator)
 *
 * Used only for the shutdown acknowledgement, where the worker
 * is about to exit and blocking is acceptable.
 * --------------------------------------------------------------- */
void mpi_response_send_sync(response_t *r)
{
  size_t total;
  char *buf = _pack_response(r, &total);
  MPI_Send(buf, (int)total, MPI_BYTE, 0 /* coordinator */,
           NCORPOS_MPI_RESPONSE_DATA, MPI_COMM_WORLD);
  free(buf);
}

/* ---------------------------------------------------------------
 * mpi_response_receive_any  –  blocking probe+recv (coordinator)
 *
 * Probes for any NCORPOS_MPI_RESPONSE_DATA message, receives it,
 * and unpacks into a heap-allocated response_t.
 * --------------------------------------------------------------- */
response_t *mpi_response_receive_any(void)
{
  MPI_Status status;
  MPI_Probe(MPI_ANY_SOURCE, NCORPOS_MPI_RESPONSE_DATA,
            MPI_COMM_WORLD, &status);

  int msg_bytes;
  MPI_Get_count(&status, MPI_BYTE, &msg_bytes);

  char *buf = malloc((size_t)msg_bytes);
  if (!buf) { perror("malloc"); exit(1); }

  MPI_Recv(buf, msg_bytes, MPI_BYTE, status.MPI_SOURCE,
           NCORPOS_MPI_RESPONSE_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

  response_t *r = calloc(1, sizeof(response_t));
  if (!r) { perror("calloc"); exit(1); }

  int *hdr          = (int *)buf;
  r->generation          = hdr[0];
  r->iteration           = hdr[1];
  r->worker_id           = hdr[2];
  r->max_worker_id       = hdr[3];
  r->first_particle      = hdr[4];
  r->num_particles_slice = hdr[5];

  if (r->num_particles_slice > 0) {
    r->particles = malloc((size_t)r->num_particles_slice * sizeof(particle_t));
    if (!r->particles) { perror("malloc"); exit(1); }
    memcpy(r->particles,
           buf + (size_t)RESPONSE_HDR_INTS * sizeof(int),
           (size_t)r->num_particles_slice * sizeof(particle_t));
  }

  free(buf);
  return r;
}
