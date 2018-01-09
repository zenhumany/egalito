#!/bin/bash

command -v wrk > /dev/null 2>&1 || { echo >&2 "needs wrk -- skipping"; exit 0; }

mkdir -p tmp
ln -sf ../../src/libegalito.so

pidfile=../binary/target/nginx/nginx/logs/nginx.pid

../../src/loader ../binary/target/nginx/nginx/sbin/nginx -c ../conf/nginx.conf >tmp/nginx.out 2>& 1 &

count=50
while [ ! -f $pidfile ]; do
  sleep 1;
  count=$((count-1))
  if [[ "$count" -eq 0 ]]; then
    echo "test failed";
    exit 1;
  fi
done
#cat $pidfile

wrk -c10 -d10s -t4 http://localhost:8000 > tmp/nginx-wrk.out

kill -QUIT $( cat $pidfile )

count=50
while [ -f $pidfile ]; do
  sleep 1;
  if [[ "$count" -eq 0 ]]; then
    echo "test failed";
    exit 1;
  fi
done

rm libegalito.so
echo "test passed"
