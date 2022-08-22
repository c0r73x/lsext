#!/bin/sh

dir="/usr/local/include"
echo "$(time ls++ -LhFXl --potsf $dir 2> /dev/null)" | grep ' system '
echo "-- ls++ -LhFXl $dir --"
echo "$(time ls -LhFXl --color=always --group-directories-first $dir 2> /dev/null)" | grep ' system '
echo "-- ls -LhFXl $dir --"
echo "$(time /bin/ls -LhFXl --color=always --group-directories-first $dir 2> /dev/null)" | grep ' system '
echo "-- /bin/ls -LhFXl $dir --"
echo "$(time lsext -N -Lnl $dir 2> /dev/null)" | grep ' system '
echo "-- lsext -N -MLnl $dir --"
echo "$(time ./build/lsext -N -Lnl $dir 2> /dev/null)" | grep ' system '
echo "-- build lsext -N -MLnl $dir --"
