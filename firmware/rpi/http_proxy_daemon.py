#!/usr/bin/env python3
"""
HTTP Proxy Daemon for GPS Hat v4

Receives HTTP requests from ESP32 over UART and forwards them to localhost.
Sends responses back to ESP32.

Protocol:
  Request:  {"type":"http_request","id":N,"method":"GET","path":"/api/...","headers":{},"body":""}
  Response: {"type":"http_response","id":N,"status":200,"headers":{},"body":"..."}

Usage:
  ./http_proxy_daemon.py [--port /dev/ttyAMA0] [--baud 115200] [--target localhost:8080]
"""

import argparse
import json
import logging
import serial
import sys
import time
import urllib.request
import urllib.error
from urllib.parse import urljoin, quote

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
log = logging.getLogger(__name__)


class HttpProxyDaemon:
    def __init__(self, port: str, baud: int, target: str):
        self.port = port
        self.baud = baud
        self.target = target if target.startswith('http') else f'http://{target}'
        self.serial = None
        self.running = False

    def connect(self) -> bool:
        """Connect to serial port."""
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.1,
                write_timeout=1.0
            )
            log.info(f"Connected to {self.port} at {self.baud} baud")
            return True
        except Exception as e:
            log.error(f"Failed to connect to {self.port}: {e}")
            return False

    def send_response(self, request_id: int, status: int, headers: dict, body: str):
        """Send HTTP response back to ESP32."""
        response = {
            "type": "http_response",
            "id": request_id,
            "status": status,
            "headers": headers,
            "body": body
        }
        line = json.dumps(response, separators=(',', ':')) + '\n'
        self.serial.write(line.encode('utf-8'))
        log.debug(f"Sent response id={request_id} status={status}")

    def handle_request(self, data: dict):
        """Handle HTTP request from ESP32."""
        request_id = data.get('id', 0)
        method = data.get('method', 'GET')
        path = data.get('path', '/')
        req_headers = data.get('headers', {})
        body = data.get('body', '')

        # URL-encode path (preserve / and ? and = and &)
        encoded_path = quote(path, safe='/?=&')
        url = urljoin(self.target, encoded_path)
        log.info(f"[{request_id}] {method} {url}")

        try:
            # Build request
            req = urllib.request.Request(url, method=method)

            # Add headers
            for key, value in req_headers.items():
                # Skip hop-by-hop headers
                if key.lower() in ('host', 'connection', 'keep-alive',
                                   'transfer-encoding', 'te', 'trailer',
                                   'upgrade', 'proxy-authorization',
                                   'proxy-authenticate'):
                    continue
                req.add_header(key, value)

            # Add body for POST/PUT/PATCH
            if body and method in ('POST', 'PUT', 'PATCH'):
                req.data = body.encode('utf-8')

            # Make request
            with urllib.request.urlopen(req, timeout=10) as resp:
                status = resp.status
                resp_headers = dict(resp.headers)
                resp_body = resp.read().decode('utf-8', errors='replace')

                # Truncate body if too large (ESP32 has limited buffer)
                if len(resp_body) > 8000:
                    resp_body = resp_body[:8000]
                    log.warning(f"[{request_id}] Response truncated to 8000 bytes")

                log.info(f"[{request_id}] Response: {status} ({len(resp_body)} bytes)")
                self.send_response(request_id, status, resp_headers, resp_body)

        except urllib.error.HTTPError as e:
            log.warning(f"[{request_id}] HTTP error: {e.code}")
            resp_body = e.read().decode('utf-8', errors='replace') if e.fp else str(e)
            self.send_response(request_id, e.code, {}, resp_body)

        except urllib.error.URLError as e:
            log.error(f"[{request_id}] URL error: {e}")
            self.send_response(request_id, 502, {}, json.dumps({"error": str(e)}))

        except Exception as e:
            log.error(f"[{request_id}] Error: {e}")
            self.send_response(request_id, 500, {}, json.dumps({"error": str(e)}))

    def process_line(self, line: str):
        """Process a line received from ESP32."""
        line = line.strip()
        if not line:
            return

        # Try to parse as JSON
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            log.debug(f"Non-JSON line: {line[:100]}")
            return

        msg_type = data.get('type', '')

        if msg_type == 'http_request':
            self.handle_request(data)
        elif msg_type == 'http_proxy':
            status = data.get('status', '')
            ssid = data.get('ssid', '')
            ip = data.get('ip', '')
            if status == 'started':
                log.info(f"HTTP Proxy started - SSID: {ssid}, IP: {ip}")
            elif status == 'stopped':
                log.info("HTTP Proxy stopped")
        elif msg_type == 'heartbeat':
            log.debug(f"Heartbeat: uptime={data.get('uptime')}s")
        elif msg_type in ('boot', 'init', 'startup', 'ready'):
            log.info(f"ESP32: {data}")
        else:
            log.debug(f"Unknown message: {data}")

    def run(self):
        """Main loop."""
        if not self.connect():
            return 1

        self.running = True
        log.info(f"HTTP Proxy Daemon started - forwarding to {self.target}")
        log.info("Waiting for HTTP requests from ESP32...")

        buffer = ""
        while self.running:
            try:
                # Read available data
                if self.serial.in_waiting > 0:
                    data = self.serial.read(self.serial.in_waiting)
                    buffer += data.decode('utf-8', errors='replace')

                    # Process complete lines
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        self.process_line(line)
                else:
                    time.sleep(0.01)

            except KeyboardInterrupt:
                log.info("Shutting down...")
                self.running = False

            except Exception as e:
                log.error(f"Error: {e}")
                time.sleep(1)

        if self.serial:
            self.serial.close()

        return 0


def main():
    parser = argparse.ArgumentParser(description='HTTP Proxy Daemon for GPS Hat v4')
    parser.add_argument('--port', '-p', default='/dev/ttyAMA0',
                        help='Serial port (default: /dev/ttyAMA0)')
    parser.add_argument('--baud', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--target', '-t', default='localhost:80',
                        help='Target server (default: localhost:80)')
    parser.add_argument('--debug', '-d', action='store_true',
                        help='Enable debug logging')

    args = parser.parse_args()

    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)

    daemon = HttpProxyDaemon(args.port, args.baud, args.target)
    sys.exit(daemon.run())


if __name__ == '__main__':
    main()
