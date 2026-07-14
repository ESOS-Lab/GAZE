#!/bin/sh
# Adds the MySQL JDBC connector jar to the classpath for all ycsb.sh invocations.
CLASSPATH="$CLASSPATH:$YCSB_HOME/mysql-connector-j-8.0.33.jar"
