#!/usr/bin/env bash

cfg_home=`pwd`
gem5_dir=${ALADDIN_HOME}/../..
bmk_dir=/home/samxi/active_projects/composable/nnet_lib/build

${gem5_dir}/build/X86/gem5.opt \
  --debug-flags=Aladdin,HybridDatapath,HybridDatapathVerbose \
  --outdir=${cfg_home}/outputs \
  ${gem5_dir}/configs/aladdin/aladdin_se.py \
  --num-cpus=1 \
  --mem-size=4GB \
  --mem-type=DDR3_1600_x64  \
  --sys-clock=1GHz \
  --cpu-type=detailed \
  --caches \
  --cacheline_size=32 \
  --record-dram-traffic \
  --accel_cfg_file=${cfg_home}/gem5.cfg \
  -c ${bmk_dir}/nnet-gem5-accel \
  -o ../../../models/mnist/minerva.conf \
  | gzip -c > stdout.gz
