#!/bin/sh
# Finder Shell Script to find the number of files in a directory containing a given string
# Author: NOOMANE Ridha

if [ $# -ne 2 ]; then
    echo "Please specify two arguments: $0 <directory> <string> "
    exit 1
fi

direcotry=$1
string=$2

if [ ! -d $direcotry ]; then
    echo "Directory not exist"
    exit 1
fi

nmatch=$(grep -rc "$string" $direcotry | awk -F ':' 'BEGIN {sum=0}{sum+=$2} END {print sum}')
nfiles=$(find $direcotry  -type f | wc -l)

echo "The number of files are $nfiles and the number of matching lines are $nmatch"
