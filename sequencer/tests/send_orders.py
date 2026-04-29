from test_client import SimpleFixClient
import time


def main():
    client = SimpleFixClient("127.0.0.1", 5001, "CLIENT", "EXCHANGE")
    client.connect()
    client.send_logon()
    # allow time for the acceptor to register the session
    time.sleep(0.5)

    symbols = ["AAPL", "MSFT", "GOOG", "TSLA", "AMZN", "NFLX"]
    for symbol in symbols:
        client.send_order(symbol, "1", 100, 150.0)
        time.sleep(0.1)

    # give the server a moment to process and log
    time.sleep(1.0)
    client.disconnect()


if __name__ == "__main__":
    main()
