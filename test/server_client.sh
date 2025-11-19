#!/bin/bash

(LD_LIBRARY_PATH=. ./bgce) > server.log 2>&1 &
sleep 2
echo "[MAKE] Server running"

echo "[MAKE] Running test client..."
#LD_LIBRARY_PATH=. ./client > client.log 2>&1

sleep 3
echo "[MAKE] Stopping server..."
killall bgce

echo "[MAKE] Test finished."

