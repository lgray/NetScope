#!/bin/bash

fName=$1
cMask=$2
nEvents=$3
pageSize=$4

LD_LIBRARY_PATH=hdf5/lib/:$LD_LIBRARY_PATH ./dpo5054 192.168.0.2 4000 $fName $cMask $nEvent $pageSize