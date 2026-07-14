#!/bin/bash

sudo bin/ycsb load jdbc -P workloads/workloada -P db.properties -p recordcount=14000000 -p insertstart=9000000 -p insertcount=5000000  -p fieldlength=800 -threads 38 -cp mysql-connector-j-8.0.33.jar # 99GB fixed size
