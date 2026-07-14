#!/bin/bash

#sudo bin/ycsb load jdbc -P workloads/workloada -P db.properties -p recordcount=9000000 -p fieldlength=800 -threads 38 -cp mysql-connector-j-8.0.33.jar # 99GB fixed size
sudo bin/ycsb load jdbc -P workloads/workloada -P db.properties -p recordcount=16000000 -p fieldlength=800 -threads 38 -cp mysql-connector-j-8.0.33.jar # 174 GB fixed size
#sudo bin/ycsb load jdbc -P workloads/workloada -P db.properties -p recordcount=10000000 -p fieldlength=800 -threads 38 -cp mysql-connector-j-8.0.33.jar # 185GB
#sudo bin/ycsb load jdbc -P workloads/workloada -P db.properties -p recordcount=1000000 -p fieldlength=800 -threads 38 -cp mysql-connector-j-8.0.33.jar # 167GB
