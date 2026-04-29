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

Plan #1 MPI tag layout:
  NCORPOS_MPI_WAKEUP          coordinator -> worker  (group-assignment)
  NCORPOS_MPI_PAYLOAD_DATA    coordinator -> worker  (subpayload bytes)
  NCORPOS_MPI_RESPONSE_DATA   worker -> coordinator  (packed response)
  NCORPOS_MPI_SHUTDOWN_WORKER coordinator -> worker  (shutdown signal)
*/
#ifndef __MPI_COMM_H_
#define __MPI_COMM_H_

#include <mpi.h>
#include "../src/ncorpos.h"

/* ---------------------------------------------------------------
 * MPI message tags
 * --------------------------------------------------------------- */
#define NCORPOS_MPI_WAKEUP           0  /* coordinator -> worker: group info    */
#define NCORPOS_MPI_PAYLOAD_DATA     1  /* coordinator -> worker: subpayload    */
#define NCORPOS_MPI_RESPONSE_DATA    2  /* worker -> coordinator: response      */
#define NCORPOS_MPI_SHUTDOWN_WORKER  3  /* coordinator -> worker: shutdown      */

/* ---------------------------------------------------------------
 * coordinator -> worker: deliver a sub-payload
 * --------------------------------------------------------------- */
void          mpi_subpayload_send    (subpayload_t *sub, int target);
subpayload_t *mpi_subpayload_receive (int source);

/* ---------------------------------------------------------------
 * worker -> coordinator: packed response (Plan #1)
 *
 * Wire format (NCORPOS_MPI_RESPONSE_DATA):
 *   6 x int   : generation, iteration, worker_id, max_worker_id,
 *               first_particle, num_particles_slice
 *   n x particle_t bytes  (n = num_particles_slice; 0 for shutdown ack)
 *
 * mpi_response_isend  - non-blocking; *buf_out must stay alive until
 *                       MPI_Wait(*req_out) completes, then caller free()s it.
 * mpi_response_send_sync - blocking (used for shutdown ack).
 * --------------------------------------------------------------- */
void mpi_response_isend    (response_t *r,
                             MPI_Request *req_out,
                             char       **buf_out);
void mpi_response_send_sync(response_t *r);

/* ---------------------------------------------------------------
 * coordinator: receive one packed response from any worker.
 * Blocks until a NCORPOS_MPI_RESPONSE_DATA message arrives.
 * --------------------------------------------------------------- */
response_t *mpi_response_receive_any(void);

#endif /* __MPI_COMM_H_ */
