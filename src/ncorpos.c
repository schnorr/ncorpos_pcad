/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

Implementation of the O(N²) N-body gravitational simulation.
Inspired by mandelbrot.c from Fractal @ PCAD.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ncorpos.h"

/* ---------------------------------------------------------------
 * Memory management
 * --------------------------------------------------------------- */

void free_payload(void *ptr)
{
  if (!ptr) return;
  payload_t *p = (payload_t *)ptr;
  free(p->particles);
  free(p);
}

void free_subpayload(void *ptr)
{
  if (!ptr) return;
  subpayload_t *s = (subpayload_t *)ptr;
  free(s->payload.particles);
  free(s);
}

void free_response(void *ptr)
{
  if (!ptr) return;
  response_t *r = (response_t *)ptr;
  free(r->particles);
  free(r);
}

/* ---------------------------------------------------------------
 * discretize_payload
 *
 * Divide payload->particles equally among num_workers workers.
 * Returns an array of subpayload_t* of length *length.
 * Each subpayload carries a *copy* of the full particle array.
 * --------------------------------------------------------------- */
subpayload_t **discretize_payload(payload_t *payload,
                                  int        num_workers,
                                  int       *length)
{
  if (!payload || !length || num_workers <= 0)
    return NULL;

  int N = payload->num_particles;
  if (num_workers > N)
    num_workers = N; /* cannot have more workers than particles */

  *length = num_workers;

  subpayload_t **ret = calloc((size_t)num_workers, sizeof(subpayload_t *));
  if (!ret) {
    perror("calloc failed");
    exit(1);
  }

  int base  = N / num_workers;
  int extra = N % num_workers; /* first `extra` workers get one extra particle */
  int start = 0;

  for (int w = 0; w < num_workers; w++) {
    int slice = base + (w < extra ? 1 : 0);

    ret[w] = calloc(1, sizeof(subpayload_t));
    if (!ret[w]) {
      perror("calloc failed");
      exit(1);
    }

    /* Copy metadata from the original payload */
    ret[w]->payload           = *payload; /* struct copy (particles ptr copied too) */
    ret[w]->payload.particles = malloc((size_t)N * sizeof(particle_t));
    if (!ret[w]->payload.particles) {
      perror("malloc failed");
      exit(1);
    }
    memcpy(ret[w]->payload.particles, payload->particles,
           (size_t)N * sizeof(particle_t));

    ret[w]->first_particle = start;
    ret[w]->last_particle  = start + slice;

    start += slice;
  }

  return ret;
}

/* ---------------------------------------------------------------
 * ncorpos_step  –  O(N²) Newton gravity, Kick-Drift-Kick Leapfrog
 *
 * The central black hole (id == 0) is kept fixed at the origin to
 * anchor the galactic potential and prevent the galaxy from
 * random-walking off-screen due to asymmetric N-body forces.
 *
 * Updates velocity and position for particles in the range
 * [sub->first_particle, sub->last_particle).
 * All particles are read for the gravitational force computation.
 * --------------------------------------------------------------- */
void ncorpos_step(subpayload_t *sub, double dt, double softening)
{
  if (!sub) return;
  if (dt        <= 0.0) dt        = NCORPOS_DT;
  if (softening <= 0.0) softening = NCORPOS_SOFTENING;

  particle_t *particles = sub->payload.particles;
  int         N         = sub->payload.num_particles;
  double      half_dt   = dt * 0.5;

  /* Step 1: half-kick with stored (old) accelerations */
  for (int i = sub->first_particle; i < sub->last_particle; i++) {
    if (particles[i].id == 0) continue; /* BH stays fixed at origin */
    particles[i].vx += particles[i].ax * half_dt;
    particles[i].vy += particles[i].ay * half_dt;
  }

  /* Step 2: drift */
  for (int i = sub->first_particle; i < sub->last_particle; i++) {
    if (particles[i].id == 0) continue; /* BH stays fixed at origin */
    particles[i].x += particles[i].vx * dt;
    particles[i].y += particles[i].vy * dt;
  }

  /* Steps 3+4+5: recompute accelerations, second half-kick, store */
  for (int i = sub->first_particle; i < sub->last_particle; i++) {
    if (particles[i].id == 0) continue; /* BH stays fixed at origin */
    double ax = 0.0, ay = 0.0;

    for (int j = 0; j < N; j++) {
      if (j == i) continue;

      double dx    = particles[j].x - particles[i].x;
      double dy    = particles[j].y - particles[i].y;
      double dist2 = dx * dx + dy * dy + softening * softening;
      double dist  = sqrt(dist2);
      double force = NCORPOS_G * particles[j].mass / dist2;

      ax += force * dx / dist;
      ay += force * dy / dist;
    }

    /* Step 4: second half-kick */
    particles[i].vx += ax * half_dt;
    particles[i].vy += ay * half_dt;
    /* Step 5: store for next step */
    particles[i].ax = ax;
    particles[i].ay = ay;
  }
}

/* ---------------------------------------------------------------
 * create_response_for_subpayload
 *
 * Takes a snapshot of the updated slice and wraps it in a
 * response_t ready for transmission.
 * --------------------------------------------------------------- */
response_t *create_response_for_subpayload(subpayload_t *sub,
                                           int           worker_id,
                                           int           max_worker_id,
                                           int           iteration)
{
  if (!sub) return NULL;

  int slice = sub->last_particle - sub->first_particle;

  response_t *r = calloc(1, sizeof(response_t));
  if (!r) {
    perror("calloc failed");
    exit(1);
  }

  r->generation          = sub->payload.generation;
  r->iteration           = iteration;
  r->worker_id           = worker_id;
  r->max_worker_id       = max_worker_id;
  r->first_particle      = sub->first_particle;
  r->num_particles_slice = slice;

  r->particles = malloc((size_t)slice * sizeof(particle_t));
  if (!r->particles) {
    perror("malloc failed");
    exit(1);
  }
  memcpy(r->particles,
         sub->payload.particles + sub->first_particle,
         (size_t)slice * sizeof(particle_t));

  return r;
}

/* ---------------------------------------------------------------
 * Debug printers
 * --------------------------------------------------------------- */

void payload_print(const char *func, const char *msg, const payload_t *p)
{
  printf("(%d) %s: %s\n", p->generation, func, msg);
  printf("\tparticles=%d  iterations=%d  workers=%d\n",
         p->num_particles, p->num_iterations, p->num_workers);
}

void response_print(const char *func __attribute__((unused)), const char *msg, const response_t *r)
{
  printf("(%d) iter=%d worker=%d/%d %s: slice=[%d, %d)\n",
         r->generation, r->iteration,
         r->worker_id, r->max_worker_id,
         msg,
         r->first_particle,
         r->first_particle + r->num_particles_slice);
  if (r->particles) {
    printf("\tfirst particle: id=%d  x=%.6e  y=%.6e  vx=%.3e  vy=%.3e\n",
           r->particles[0].id,
           r->particles[0].x,  r->particles[0].y,
           r->particles[0].vx, r->particles[0].vy);
  }
}
