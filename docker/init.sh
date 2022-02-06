#!/bin/bash
sleep 2
tgtadm --op update --mode sys --name State -v offline
tgtadm --op update --mode sys --name State -v ready
tgt-admin -e