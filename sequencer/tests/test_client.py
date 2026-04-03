import socket
import time
import threading
from datetime import datetime
import queue


class SimpleFixClient:
    def __init__(self, host, port, sender_comp_id, target_comp_id):
        self.host = host
        self.port = port
        self.sender = sender_comp_id
        self.target = target_comp_id
        self.seq_num = 1
        self.sock = None
        self.received_messages = []
        self._recv_lock = threading.Lock()
        self._stop_reader = threading.Event()
        self._reader_thread = None

    def _get_timestamp(self):
        return datetime.utcnow().strftime("%Y%m%d-%H:%M:%S.000")

    def _construct_message(self, msg_type, body_tags):
        body = f"35={msg_type}|49={self.sender}|56={self.target}|34={self.seq_num}|52={self._get_timestamp()}|"
        for k, v in body_tags.items():
            body += f"{k}={v}|"
        msg = f"8=FIX.4.2|9={len(body)}|{body}"
        raw_msg = msg.replace('|', '\x01')
        checksum_val = sum(ord(c) for c in raw_msg) % 256
        checksum_str = f"{checksum_val:03d}"
        final_msg = f"{raw_msg}10={checksum_str}\x01"
        self.seq_num += 1
        return final_msg

    def _background_reader(self):
        """
        Runs in a background thread, continuously reads from the socket
        and buffers data. Parses complete FIX messages delimited by SOH (\x01)
        on tag 10 (checksum = end of message).
        """
        self.sock.settimeout(1.0)
        buf = ""
        while not self._stop_reader.is_set():
            try:
                chunk = self.sock.recv(4096).decode("ascii", errors="replace")
                if not chunk:
                    break
                buf += chunk
                while True:
                    end = buf.find("10=")
                    if end == -1:
                        break
                    soh = buf.find("\x01", end)
                    if soh == -1:
                        break
                    full_msg = buf[:soh + 1]
                    buf = buf[soh + 1:]
                    
                    readable = full_msg.replace("\x01", "|").rstrip("|")
                    msg_type = self._extract_tag(full_msg, "35")
                    
                    parsed_msg = {"raw": full_msg, "readable": readable, "msg_type": msg_type}
                    # Extract common tags
                    for tag in ["11", "37", "39", "55", "54", "38", "44"]:
                        val = self._extract_tag(full_msg, tag)
                        if val:
                            parsed_msg[tag] = val
                            
                    with self._recv_lock:
                        self.received_messages.append(parsed_msg)
            except socket.timeout:
                continue
            except Exception:
                if not self._stop_reader.is_set():
                    break
                break

    def _extract_tag(self, raw_msg, tag):
        """Extract a tag value from a raw SOH-delimited FIX message."""
        marker = f"\x01{tag}=" if tag not in ["8", "35"] else f"{tag}="
        if tag == "35" and raw_msg.startswith("8=FIX"): # 35 is usually after 9
             marker = f"\x01{tag}="
        
        start = raw_msg.find(marker)
        if start == -1:
            # Try without leading SOH if it's the very first tag (8)
            if tag == "8":
                marker = f"{tag}="
                start = raw_msg.find(marker)
            if start == -1:
                return ""
        
        start += len(marker)
        end = raw_msg.find("\x01", start)
        return raw_msg[start:end] if end != -1 else raw_msg[start:]

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self._reader_thread = threading.Thread(target=self._background_reader, daemon=True)
        self._reader_thread.start()

    def disconnect(self):
        self._stop_reader.set()
        if self.sock:
            self.sock.close()
        if self._reader_thread:
            self._reader_thread.join(timeout=2)

    def send_logon(self):
        logon_tags = {"98": "0", "108": "30"}
        msg = self._construct_message("A", logon_tags)
        self.sock.sendall(msg.encode())

    def send_order(self, symbol, side, qty, price=None):
        cl_ord_id = f"ORD{int(time.time() * 1000)}"
        order_tags = {
            "11": cl_ord_id,
            "21": "1",
            "55": symbol,
            "54": side,
            "60": self._get_timestamp(),
            "38": str(qty),
            "40": "2" if price else "1",
        }
        if price:
            order_tags["44"] = str(price)
        msg = self._construct_message("D", order_tags)
        self.sock.sendall(msg.encode())
        return cl_ord_id

    def wait_for_message(self, msg_type, timeout=5):
        start_time = time.time()
        while time.time() - start_time < timeout:
            with self._recv_lock:
                for msg in self.received_messages:
                    if msg["msg_type"] == msg_type:
                        return msg
            time.sleep(0.1)
        return None