#!/bin/sh

dir="/usr/include"
echo "$(time ls++ -LhFX --potsf $dir 2> /dev/null)" | grep ' system '
echo "-- ls++ $dir --"
echo "$(time ls -LhFX --color=always --group-directories-first $dir 2> /dev/null)" | grep ' system '
echo "-- ls $dir --"
echo "$(time /bin/ls -LhFX --color=always --group-directories-first $dir 2> /dev/null)" | grep ' system '
echo "-- /bin/ls $dir --"
echo "$(time ./build/lsext $dir 2> /dev/null)" | grep ' system '
echo "-- lsext $dir --"
