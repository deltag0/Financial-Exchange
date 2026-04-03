import time
import pytest
import re

def parse_shard_logs(log_path):
    """
    Parses the sequencer log to extract (Ticker -> Shard) mapping.
    Expected log format: 
    [Sequencer] Processing message... shard: 0 (Ticker: AAPL)
    """
    ticker_to_shard = {}
    with open(log_path, 'r') as f:
        for line in f:
            if "Processing message" in line and "shard:" in line:
                match = re.search(r"shard: (\d+) \(Ticker: ([^\)]+)\)", line)
                if match:
                    shard_id = int(match.group(1))
                    ticker = match.group(2)
                    if ticker not in ticker_to_shard:
                        ticker_to_shard[ticker] = set()
                    ticker_to_shard[ticker].add(shard_id)
    return ticker_to_shard

def test_sharding_consistency(sequencer_process, fix_client):
    """
    Verify that multiple orders for the same symbol are always routed to the same shard.
    """
    proc, log_path = sequencer_process
    client = fix_client
    
    symbols = ["AAPL", "MSFT", "GOOG", "TSLA"]
    rounds = 3
    
    # Send orders multiple times for each symbol
    for _ in range(rounds):
        for symbol in symbols:
            client.send_order(symbol, "1", 100, 150.0)
            time.sleep(0.1)
            
    # Give sequencer time to finish processing and logging
    time.sleep(2)
    
    # Parse logs and verify consistency
    ticker_to_shards = parse_shard_logs(log_path)
    
    # Check that each ticker is mapped to EXACTLY ONE shard
    for ticker, shards in ticker_to_shards.items():
        if ticker in symbols:
            assert len(shards) == 1, f"Ticker {ticker} was seen on multiple shards: {shards}"

def test_shard_distribution(sequencer_process, fix_client):
    """
    Verify that orders for different symbols are distributed across different shards.
    Note: With 4 shards and 8 symbols, we expect to use most if not all shards.
    """
    proc, log_path = sequencer_process
    client = fix_client
    
    symbols = ["AAPL", "MSFT", "GOOG", "TSLA", "AMZN", "NFLX", "META", "NVDA"]
    
    # Send orders for many symbols
    for symbol in symbols:
        client.send_order(symbol, "1", 100, 150.0)
        time.sleep(0.1)
        
    # Give sequencer time to finish processing and logging
    time.sleep(2)
    
    # Parse logs and verify distribution
    ticker_to_shards = parse_shard_logs(log_path)
    
    all_used_shards = set()
    for shards in ticker_to_shards.values():
        all_used_shards.update(shards)
        
    print(f"Used shards: {all_used_shards}")
    
    # We have 4 shards in the sequencer (hardcoded in main.cpp typically)
    # With 8 symbols, we should be using at least 2 or 3 shards unless hash is very unlucky.
    assert len(all_used_shards) > 1, f"Only one shard was used for {len(symbols)} symbols: {all_used_shards}"
