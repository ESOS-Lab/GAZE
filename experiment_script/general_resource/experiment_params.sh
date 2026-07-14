#!/bin/bash
# READ-ONLY reference copy of the paper's original workload-intensity
# parameters (record counts, thread counts, runtime, etc.).
#
# Never edited by set_params.sh or anything else — it's the restore point
# for set_params.sh's "reset to paper config" option, which copies this
# file over the live experiment_params.sh. If you need to change what
# "paper config" means, edit this file by hand (and experiment_params.sh
# to match, if you want the change to take effect immediately too).

# ---- FIO (Fig-15/17) ----
FIO_BLOCK_SIZE=4k
FIO_IO_TYPE=randwrite
FIO_NUM_JOBS=1
FIO_RUNTIME=1200        # seconds
FIO_FILESIZE=180GB
FIO_GC_START_SEC=500     # fio records stats only after this second (GC onset on the
                         # paper's 256G SSD). Fed to the patched fio via env; the
                         # kiops_avg_gc tail is derived from RUNTIME-this in run_fio.sh.

# ---- Fileserver / filebench (Fig-16/18) ----
FILESERVER_NFILES=1800000
FILESERVER_MEANDIRWIDTH=20
FILESERVER_NTHREADS=16
FILESERVER_IOSIZE=1m
FILESERVER_MEANAPPENDSIZE=4k
FILESERVER_RUNTIME=1600 # seconds
FILESERVER_GC_START_SEC=100 # filebench's GC-window stats ("GC IO Summary") start at
                            # this second (GC onset). Fed to filebench via env.

# ---- YCSB-A: update-heavy (Fig-16/18) ----
YCSBA_RECORDSIZE=8000    # bytes; fieldlength = this / 10
YCSBA_RECORDCOUNT=16000000
YCSBA_OPERATIONCOUNT=10000000
YCSBA_THREADS=15

# ---- YCSB-F: read-modify-write (Fig-16/18) ----
YCSBF_RECORDSIZE=8000
YCSBF_RECORDCOUNT=16000000
YCSBF_OPERATIONCOUNT=10000000
YCSBF_THREADS=15

# ---- TPC-C (Fig-16/18) ----
TPCC_WAREHOUSES=1700
TPCC_CLIENTS=15
TPCC_RAMPUP=10           # seconds (tpcc_start -r) -- warmup, not measured
TPCC_DURATION=1800       # seconds (tpcc_start -l) -- the measured window
