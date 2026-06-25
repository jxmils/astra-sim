module purge
module load GCC/11.3.0
module load CMake/3.24.3-GCCcore-11.3.0
module load Boost/1.79.0-GCC-11.3.0
module load protobuf/3.19.4-GCCcore-11.3.0
module load OpenMPI/4.1.4-GCC-11.3.0

export ASTRA_SIM=/data/engs-glass/catz0932/astra-sim
export VENV_DIR="${ASTRA_SIM}/.venv-astra"
source "${VENV_DIR}/bin/activate"