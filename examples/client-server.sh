#!/usr/bin/env bash

# run a server
th client-server.lua --server 4 &

# run 4 clients
th client-server.lua &
th client-server.lua &
th client-server.lua &
th client-server.lua &

# wait for them all
wait
