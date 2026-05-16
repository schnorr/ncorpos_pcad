#!/bin/bash
#SBATCH --nodes=6
#SBATCH --partition=cei
#SBATCH --time=02:00:00
#SBATCH --job-name="ncorpos"
#SBATCH --output=%x_%j.out
#SBATCH --error=%x_%j.err

# fail on error
# https://gist.github.com/mohanpedala/1e2ff5661761d3abd0385e8223e16425
set -euxo pipefail

# Basic configuration
BASEEXPDIR=$HOME/pa_workspace/
NCORPOSDIR=${BASEEXPDIR}/ncorpos_pcad/
KEY=${SLURM_JOB_NAME}_${SLURM_JOB_ID}


# Ncorpos app compile
{
    pushd ${NCORPOSDIR}
    COMPILE_SCRIPT=./compile_within_guix_env.sh
    cat > ${COMPILE_SCRIPT} << EOF
    ldd --version
    mpicc --version
    make clean
    # disable grafica and textual compilation
    sed -i 's/all: grafica coordinator textual/all: coordinator/' Makefile
    sed -i 's/-g -fsanitize=address/-O3/' Makefile
    # compile
    make coordinator
EOF
    chmod 755 ${COMPILE_SCRIPT}
    ${COMPILE_SCRIPT}
    popd
}

hostname

# Create the hostfile
HOSTFILE="${KEY}.nodes"
srun -l hostname | sort -n | awk '{print $2}' | sed 's/$/ slots=24/' > $HOSTFILE

#   --bind-to nonenone \
#   -mca btl_base_verbose 30 \
#   -mca pml_base_verbose 30 \
#   -mca oob_base_verbose 30 \

# Run
#env | awk -F= '{print "-x " $1}' | xargs \
RUN_SCRIPT=./run_within_guix_env.sh
cat > ${RUN_SCRIPT} << EOF
ldd --version
mpirun --version
mpirun \
    --mca oob_tcp_if_include 192.168.30.0/24 \
    --mca btl_tcp_if_include 192.168.30.0/24 \
    --np 144 \
    --hostfile $HOSTFILE \
    ${NCORPOSDIR}/bin/coordinator \
    9191
EOF
chmod 755 ${RUN_SCRIPT}
${RUN_SCRIPT}
echo "terminou"
