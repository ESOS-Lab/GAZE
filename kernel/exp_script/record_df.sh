#!/bin/bash

touch $1
for (( ; ; )) 
do 
	df -h | grep nvme0n1p1 >> $1
	sleep 1 
done



