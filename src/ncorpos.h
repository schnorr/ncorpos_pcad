/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

"NCorpos @ PCAD" is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with "NCorpos @ PCAD". If not, see
<https://www.gnu.org/licenses/>.
*/
#ifndef __NCORPOS_H_
#define __NCORPOS_H_

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

/* ---------------------------------------------------------------
 * Physical / simulation constants (defaults; can be overridden)
 * --------------------------------------------------------------- */
#define NCORPOS_G          6.674e-11  /* gravitational constant (SI) */
#define NCORPOS_DT         1.0e1      /* time step in seconds        */
#define NCORPOS_SOFTENING  1.0e8      /* softening length (m)        */

/* Acceptable bounds for client parameters */
#define NCORPOS_MIN_PARTICLES  2
#define NCORPOS_MAX_PARTICLES  10000
#define NCORPOS_MIN_MASS       1.0e20   /* kg */
#define NCORPOS_MAX_MASS       2.0e30   /* kg (≈1 solar mass)         */
#define NCORPOS_MIN_SPEED     -1.0e3   /* m/s */
#define NCORPOS_MAX_SPEED      1.0e3   /* m/s */

/* ---------------------------------------------------------------
 * Core data types
 * --------------------------------------------------------------- */

/* A single particle in 2-D space */
typedef struct {
  int    id;      /* unique integer identifier                     */
  double x, y;   /* position  (metres)                            */
  double vx, vy; /* velocity  (m/s)                               */
  double mass;   /* mass      (kg)                                */
} particle_t;

/* ---------------------------------------------------------------
 * Communication protocol between client and coordinator
 * --------------------------------------------------------------- */

/*
 * payload_t  –  sent by the client to the coordinator every time
 *               a new simulation is requested.
 *
 * Over TCP the struct is transmitted in two parts:
 *   1) the fixed-size payload_t   (sizeof(payload_t) bytes)
 *   2) num_particles * sizeof(particle_t) bytes  (particle array)
 *
 * The `particles` pointer is meaningless on the receiving side
 * and must be re-allocated + received separately.
 */
typedef struct {
  int    generation;    /* monotonically increasing request counter  */
  int    num_particles; /* total number of particles                 */
  int    num_iterations;/* how many simulation steps to perform      */
  int    num_workers;   /* how many MPI workers to use               */
  double space_width;   /* physical width  of the simulation domain  */
  double space_height;  /* physical height of the simulation domain  */
  int    screen_width;  /* screen width  in pixels                   */
  int    screen_height; /* screen height in pixels                   */
  particle_t *particles;/* dynamic array – NOT embedded in the struct*/
} payload_t;

/*
 * subpayload_t  –  produced by the coordinator's discretisation
 *                  step; one per MPI worker.
 *
 * Contains the full particle array plus the slice [first_particle,
 * last_particle) that this specific worker is responsible for.
 */
typedef struct {
  payload_t payload;    /* full payload (all particles + metadata)   */
  int first_particle;   /* index of first particle in worker's slice */
  int last_particle;    /* index past the last particle (exclusive)  */
} subpayload_t;

/*
 * response_t  –  produced by each MPI worker after every iteration
 *                and forwarded by the coordinator to the client.
 *
 * Over TCP:
 *   1) sizeof(response_t) bytes (fixed header; particles ptr garbage)
 *   2) num_particles_slice * sizeof(particle_t) bytes
 */
typedef struct {
  int generation;          /* matches the originating payload          */
  int iteration;           /* iteration index (0-based)                */
  int worker_id;           /* MPI rank of the sending worker (1-based) */
  int max_worker_id;       /* total number of workers                  */
  int first_particle;      /* slice start index                        */
  int num_particles_slice; /* slice length                             */
  particle_t *particles;   /* updated particle data – dynamic          */
} response_t;

/* ---------------------------------------------------------------
 * Special generation values (poison pills)
 * --------------------------------------------------------------- */
#define PAYLOAD_GENERATION_DONE     -1  /* end of a round (logging)  */
#define PAYLOAD_GENERATION_SHUTDOWN -2  /* full shutdown signal       */

/* ---------------------------------------------------------------
 * Memory management helpers
 * --------------------------------------------------------------- */
void free_payload    (void *ptr);
void free_subpayload (void *ptr);
void free_response   (void *ptr);

/* ---------------------------------------------------------------
 * Protocol / simulation functions
 * --------------------------------------------------------------- */

/* Divide payload->particles equally among num_workers workers.
 * Returns an array of subpayload_t* of length *length (= num_workers). */
subpayload_t **discretize_payload (payload_t *payload,
                                   int        num_workers,
                                   int       *length);

/* Advance one simulation step for the particles in sub's slice.
 * All particles in sub->payload.particles are read (for force
 * computation); only the slice [first_particle, last_particle)
 * is updated.  dt and softening use the module defaults when 0
 * is passed. */
void ncorpos_step (subpayload_t *sub, double dt, double softening);

/* Build a response_t snapshot from the current state of sub's slice. */
response_t *create_response_for_subpayload (subpayload_t *sub,
                                            int           worker_id,
                                            int           max_worker_id,
                                            int           iteration);

/* Debug printers */
void payload_print  (const char *func, const char *msg, const payload_t  *p);
void response_print (const char *func, const char *msg, const response_t *r);

#endif /* __NCORPOS_H_ */
