#!/bin/bash
#SBATCH -J HNSW-TEST-10M
#SBATCH -o slurms/slurm-%j.out 
#SBATCH --account=carney-bkimia-condo
#SBATCH -p batch
### SBATCH -p bigmem
#SBATCH -C intel|skylake
#SBATCH -N 1
#SBATCH -n 32
#SBATCH --mem=64G
#SBATCH --time=24:00:00

module load anaconda/3-5.2.0
source activate ~/scratch/condas/faiss/

cd /users/cfoste18/data/cfoste18/SISAP2023/indexing-challenge/submission/sisap-2023

export PYTHONUNBUFFERED=TRUE

echo python3 search/search-hnsw.py --size 10M --index HNSW-M-10-EFC-200.bin
python3 search/search-hnsw.py --size 10M --index HNSW-M-10-EFC-200.bin

source deactivate