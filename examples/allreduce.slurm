#!/bin/bash

#SBATCH --job-name=TorchIPC
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=2

srun th allreduce_slurm.lua

wait
