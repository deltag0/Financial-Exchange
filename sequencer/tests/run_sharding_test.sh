#!/bin/bash
set -e

# 1. Start the exchange in the background
echo "[Test] Starting sequencer_app..."
/app/build/sequencer_app > /app/exchange_test.log 2>&1 &
EXCHANGE_PID=$!

# Wait for it to start
sleep 2

# 2. Run the test client
echo "[Test] Running test_client.py..."
python3 /app/sequencer/tests/test_client.py

# Give it a second to process the last messages
sleep 2

# 3. Stop the exchange
echo "[Test] Stopping sequencer_app..."
kill $EXCHANGE_PID

# 4. Display the results
echo "[Test] --- Shard Distribution Results ---"
grep "Processing message" /app/exchange_test.log | sed 's/.*shard: \([0-9]*\) (Ticker: \([^)]*\))/\2 -> Shard \1/' | sort | uniq -c
