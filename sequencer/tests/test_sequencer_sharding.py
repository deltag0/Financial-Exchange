import time
import pytest
import re


def wait_for_expected_tickers(log_path, expected_tickers, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ticker_to_shard = parse_shard_logs(log_path)
        if expected_tickers.issubset(ticker_to_shard.keys()):
            return ticker_to_shard
        time.sleep(0.1)
    return parse_shard_logs(log_path)

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
    Verify that multiple orders for the configured instrument are always routed to the same shard.
    """
    proc, log_path = sequencer_process
    client = fix_client
    
    symbols = ["SPY"]
    rounds = 3
    
    # Send orders multiple times for each symbol
    for _ in range(rounds):
        for symbol in symbols:
            client.send_order(symbol, "1", 100, 150.0)
            time.sleep(0.1)

    ticker_to_shards = wait_for_expected_tickers(log_path, set(symbols))
    
    # Check that each ticker is mapped to EXACTLY ONE shard
    for ticker, shards in ticker_to_shards.items():
        if ticker in symbols:
            assert len(shards) == 1, f"Ticker {ticker} was seen on multiple shards: {shards}"

def test_configured_instrument_is_observed(sequencer_process, fix_client):
    """
    Verify that the configured instrument is processed and appears in sequencer logs.
    """
    proc, log_path = sequencer_process
    client = fix_client
    
    symbols = ["SPY"]
    
    # Send orders for many symbols
    for symbol in symbols:
        client.send_order(symbol, "1", 100, 150.0)
        time.sleep(0.1)

    ticker_to_shards = wait_for_expected_tickers(log_path, set(symbols))

    assert set(symbols).issubset(ticker_to_shards.keys())
    for ticker in symbols:
        assert len(ticker_to_shards[ticker]) == 1, f"Ticker {ticker} was seen on multiple shards: {ticker_to_shards[ticker]}"


def test_configured_order_message_is_processed(sequencer_process, fix_client):
    """
    Smoke test for the continuous FixTask loop: once orders are sent, they should
    show up in sequencer logs without relying on a fixed sleep window.
    """
    proc, log_path = sequencer_process
    client = fix_client

    symbols = ["SPY"]
    for symbol in symbols:
        client.send_order(symbol, "1", 25, 42.0)

    ticker_to_shards = wait_for_expected_tickers(log_path, set(symbols))

    assert set(symbols).issubset(ticker_to_shards.keys())
