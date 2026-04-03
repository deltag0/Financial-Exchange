import pytest
import subprocess
import time
import os
import signal
from .test_client import SimpleFixClient

@pytest.fixture(scope="session")
def sequencer_process():
    """Starts the sequencer_app and ensures it is cleaned up."""
    # Build path to the executable
    executable = "/app/build/sequencer_app"
    if not os.path.exists(executable):
        pytest.fail(f"sequencer_app not found at {executable}. Ensure it is built.")

    # Start the process and capture output to a file for later parsing
    log_file = open("sequencer_test.log", "w")
    proc = subprocess.Popen(
        [executable],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid
    )
    
    # Wait for the app to initialize (FIX acceptor startup)
    time.sleep(2)
    
    yield proc, "sequencer_test.log"
    
    # Cleanup: kill the process group to ensure all sub-threads/processes are gone
    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    proc.wait(timeout=5)
    log_file.close()

@pytest.fixture
def fix_client():
    """Provides a logged-on FIX client."""
    client = SimpleFixClient("127.0.0.1", 5001, "CLIENT", "EXCHANGE")
    client.connect()
    client.send_logon()
    
    # Wait for logon response (Heartbeat or TestRequest or just some time)
    # SimpleFixClient doesn't strictly need to wait for Logon ACK to send orders
    # but it's better practice. 
    time.sleep(1) 
    
    yield client
    
    client.disconnect()
