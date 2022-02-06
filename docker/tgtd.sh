#!/bin/bash
echo "Start"
./docker/init.sh &
tgtd -f -D -d 1

