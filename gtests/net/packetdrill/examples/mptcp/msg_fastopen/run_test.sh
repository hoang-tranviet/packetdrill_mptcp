#!/bin/bash
set -e

source ~/high_tolerance

function find_string() {
	for x in `seq 2 $#`; do
		if [[ $1 == *${!x}* ]]
		then
			return 0
		fi
	done

	return 1
}

for f in `find . -name "*.pkt" | sort`; do
  echo "Running $f ..."
  ip tcp_metrics flush all > /dev/null 2>&1
  TOLERANCE=40000
  if find_string $f $HIGH_TOLERANCE ; then
	TOLERANCE=220000
	echo "High tolerance test"
  fi
  packetdrill --tolerance_usecs=$TOLERANCE $f
done

