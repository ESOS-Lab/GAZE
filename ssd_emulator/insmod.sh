#!/bin/bash
#make

# JW SOSP26 Server !!!
#sudo insmod ./nvmev.ko memmap_start=262144M memmap_size=261121M cpus=37,39 
# 1 MByte should be reserved in the SSD size for metadta. That's why memmap_size is set to  262144 - 1023 MB (discard 1zone and use its 1MB for 1MB metadata so subtract 1023MB). 

# config.sh: SSD=256G DRAM=64G (2026-07-10)
sudo insmod ./nvmev.ko memmap_start=262144M memmap_size=261121M cpus=37,39
