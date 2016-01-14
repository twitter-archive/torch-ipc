#!/usr/bin/env bash

# run 4 nodes in a slow mode
th allreduce.lua --numNodes 4 --node 1 --base 4 &
th allreduce.lua --numNodes 4 --node 2 --base 4 &
th allreduce.lua --numNodes 4 --node 3 --base 4 &
th allreduce.lua --numNodes 4 --node 4 --base 4 &

# wait for them all
wait

# run 4 nodes in a fast mode
th allreduce.lua --numNodes 4 --node 1 &
th allreduce.lua --numNodes 4 --node 2 &
th allreduce.lua --numNodes 4 --node 3 &
th allreduce.lua --numNodes 4 --node 4 &

# wait for them all
wait
