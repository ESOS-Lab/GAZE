#!/bin/bash
cp /var/log/kern.log ./
chown -R "${SUDO_USER:-$USER}" kern.log
