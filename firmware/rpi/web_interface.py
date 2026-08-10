#!/usr/bin/env python3
"""
GPS Hat v4 - Web Interface

Simple web server for:
- Viewing WiFi and cellular connection status
- Provisioning new WiFi credentials

Runs on port 80 and is accessed via ESP32 HTTP proxy.
"""

import json
import os
import re
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

PORT = 80


def run_cmd(cmd: list, timeout: int = 5) -> tuple[int, str, str]:
    """Run command and return (returncode, stdout, stderr)."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except Exception as e:
        return -1, "", str(e)


def get_wifi_status() -> dict:
    """Get current WiFi connection status."""
    status = {
        "connected": False,
        "ssid": "",
        "ip": "",
        "signal": "",
        "frequency": "",
        "interface": "wlan0"
    }

    # Check if connected
    code, out, _ = run_cmd(["iwgetid", "-r"])
    if code == 0 and out.strip():
        status["connected"] = True
        status["ssid"] = out.strip()

    # Get IP address
    code, out, _ = run_cmd(["hostname", "-I"])
    if code == 0:
        ips = out.strip().split()
        for ip in ips:
            if ip.startswith("192.168.") or ip.startswith("10.") or ip.startswith("172."):
                status["ip"] = ip
                break

    # Get signal strength
    code, out, _ = run_cmd(["iwconfig", "wlan0"])
    if code == 0:
        # Parse signal level
        match = re.search(r'Signal level[=:](-?\d+)', out)
        if match:
            status["signal"] = f"{match.group(1)} dBm"
        # Parse frequency
        match = re.search(r'Frequency[=:](\d+\.?\d*)', out)
        if match:
            status["frequency"] = f"{match.group(1)} GHz"

    return status


def get_cellular_status() -> dict:
    """Get cellular modem status."""
    status = {
        "connected": False,
        "interface": "",
        "ip": "",
        "signal": "",
        "operator": "",
        "technology": ""
    }

    # Check for common cellular interfaces
    for iface in ["wwan0", "usb0", "eth1", "ppp0"]:
        code, out, _ = run_cmd(["ip", "addr", "show", iface])
        if code == 0 and "inet " in out:
            status["interface"] = iface
            match = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', out)
            if match:
                status["ip"] = match.group(1)
                status["connected"] = True
            break

    # Try to get modem info via mmcli (ModemManager)
    code, out, _ = run_cmd(["mmcli", "-L"])
    if code == 0 and "/Modem/" in out:
        # Extract modem number
        match = re.search(r'/Modem/(\d+)', out)
        if match:
            modem_num = match.group(1)
            code, out, _ = run_cmd(["mmcli", "-m", modem_num])
            if code == 0:
                # Parse operator
                match = re.search(r"operator name: '([^']+)'", out)
                if match:
                    status["operator"] = match.group(1)
                # Parse signal
                match = re.search(r"signal quality: '(\d+)'", out)
                if match:
                    status["signal"] = f"{match.group(1)}%"
                # Parse access tech
                match = re.search(r"access tech: '([^']+)'", out)
                if match:
                    status["technology"] = match.group(1)

    return status


def get_saved_networks() -> list:
    """Get list of saved WiFi networks from wpa_supplicant."""
    networks = []
    try:
        code, out, _ = run_cmd(["wpa_cli", "-i", "wlan0", "list_networks"])
        if code == 0:
            lines = out.strip().split('\n')
            for line in lines[1:]:  # Skip header
                parts = line.split('\t')
                if len(parts) >= 2:
                    networks.append({
                        "id": parts[0],
                        "ssid": parts[1],
                        "current": "[CURRENT]" in line
                    })
    except Exception:
        pass
    return networks


def scan_wifi_networks() -> list:
    """Scan for available WiFi networks."""
    networks = []

    # Trigger scan
    run_cmd(["wpa_cli", "-i", "wlan0", "scan"])

    # Get results
    code, out, _ = run_cmd(["wpa_cli", "-i", "wlan0", "scan_results"])
    if code == 0:
        lines = out.strip().split('\n')
        seen = set()
        for line in lines[1:]:  # Skip header
            parts = line.split('\t')
            if len(parts) >= 5:
                ssid = parts[4]
                if ssid and ssid not in seen:
                    seen.add(ssid)
                    networks.append({
                        "ssid": ssid,
                        "signal": parts[2],
                        "security": "WPA" if "WPA" in parts[3] else "Open"
                    })

    # Sort by signal strength
    networks.sort(key=lambda x: int(x["signal"]) if x["signal"].lstrip('-').isdigit() else 0, reverse=True)
    return networks[:10]  # Return top 10


def add_wifi_network(ssid: str, password: str) -> tuple[bool, str]:
    """Add a new WiFi network and connect to it."""
    if not ssid:
        return False, "SSID is required"

    # Add network
    code, out, _ = run_cmd(["wpa_cli", "-i", "wlan0", "add_network"])
    if code != 0:
        return False, "Failed to add network"

    network_id = out.strip()

    # Set SSID
    code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "set_network", network_id, "ssid", f'"{ssid}"'])
    if code != 0:
        return False, "Failed to set SSID"

    # Set password or key_mgmt for open networks
    if password:
        code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "set_network", network_id, "psk", f'"{password}"'])
        if code != 0:
            return False, "Failed to set password"
    else:
        code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "set_network", network_id, "key_mgmt", "NONE"])

    # Enable network
    code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "enable_network", network_id])
    if code != 0:
        return False, "Failed to enable network"

    # Save configuration
    code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "save_config"])

    # Select this network
    code, _, _ = run_cmd(["wpa_cli", "-i", "wlan0", "select_network", network_id])

    return True, f"Network '{ssid}' added and connecting..."


HTML_TEMPLATE = """<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>GPS Hat - Network Config</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #1a1a2e;
            color: #eee;
            padding: 20px;
            max-width: 600px;
            margin: 0 auto;
        }
        h1 { color: #4ecca3; margin-bottom: 20px; font-size: 1.5em; }
        h2 { color: #4ecca3; margin: 20px 0 10px; font-size: 1.2em; border-bottom: 1px solid #333; padding-bottom: 5px; }
        .card {
            background: #16213e;
            border-radius: 8px;
            padding: 15px;
            margin-bottom: 15px;
        }
        .status-row {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid #333;
        }
        .status-row:last-child { border-bottom: none; }
        .label { color: #888; }
        .value { color: #fff; font-weight: 500; }
        .connected { color: #4ecca3; }
        .disconnected { color: #ff6b6b; }
        form { margin-top: 15px; }
        label { display: block; margin-bottom: 5px; color: #888; }
        input[type="text"], input[type="password"], select {
            width: 100%;
            padding: 12px;
            border: 1px solid #333;
            border-radius: 6px;
            background: #0f0f23;
            color: #fff;
            margin-bottom: 15px;
            font-size: 16px;
        }
        button {
            width: 100%;
            padding: 12px;
            background: #4ecca3;
            color: #1a1a2e;
            border: none;
            border-radius: 6px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
        }
        button:hover { background: #3db892; }
        .message {
            padding: 10px;
            border-radius: 6px;
            margin-bottom: 15px;
        }
        .message.success { background: #1e4d3d; color: #4ecca3; }
        .message.error { background: #4d1e1e; color: #ff6b6b; }
        .scan-btn {
            background: #333;
            color: #fff;
            margin-bottom: 15px;
        }
        .network-list {
            max-height: 200px;
            overflow-y: auto;
        }
        .network-item {
            padding: 10px;
            border-bottom: 1px solid #333;
            cursor: pointer;
        }
        .network-item:hover { background: #1e3a5f; }
        .network-item .ssid { font-weight: 500; }
        .network-item .meta { font-size: 0.85em; color: #888; }
    </style>
</head>
<body>
    <h1>GPS Hat Network Config</h1>

    {message}

    <h2>WiFi Status</h2>
    <div class="card">
        <div class="status-row">
            <span class="label">Status</span>
            <span class="value {wifi_status_class}">{wifi_status_text}</span>
        </div>
        <div class="status-row">
            <span class="label">Network</span>
            <span class="value">{wifi_ssid}</span>
        </div>
        <div class="status-row">
            <span class="label">IP Address</span>
            <span class="value">{wifi_ip}</span>
        </div>
        <div class="status-row">
            <span class="label">Signal</span>
            <span class="value">{wifi_signal}</span>
        </div>
    </div>

    <h2>Cellular Status</h2>
    <div class="card">
        <div class="status-row">
            <span class="label">Status</span>
            <span class="value {cell_status_class}">{cell_status_text}</span>
        </div>
        <div class="status-row">
            <span class="label">Operator</span>
            <span class="value">{cell_operator}</span>
        </div>
        <div class="status-row">
            <span class="label">Technology</span>
            <span class="value">{cell_technology}</span>
        </div>
        <div class="status-row">
            <span class="label">IP Address</span>
            <span class="value">{cell_ip}</span>
        </div>
        <div class="status-row">
            <span class="label">Signal</span>
            <span class="value">{cell_signal}</span>
        </div>
    </div>

    <h2>Connect to WiFi</h2>
    <div class="card">
        <form method="POST" action="/wifi/connect">
            <label for="ssid">Network Name (SSID)</label>
            <input type="text" id="ssid" name="ssid" placeholder="Enter WiFi name" required>

            <label for="password">Password</label>
            <input type="password" id="password" name="password" placeholder="Enter password (leave empty for open networks)">

            <button type="submit">Connect</button>
        </form>
    </div>

    <h2>Available Networks</h2>
    <div class="card">
        <form method="GET" action="/wifi/scan">
            <button type="submit" class="scan-btn">Scan for Networks</button>
        </form>
        <div class="network-list">
            {available_networks}
        </div>
    </div>

    <script>
        // Click on network to fill in SSID
        document.querySelectorAll('.network-item').forEach(item => {
            item.addEventListener('click', () => {
                document.getElementById('ssid').value = item.dataset.ssid;
                document.getElementById('password').focus();
            });
        });
    </script>
</body>
</html>
"""


class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[HTTP] {args[0]}")

    def send_html(self, html: str, status: int = 200):
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", len(html.encode()))
        self.end_headers()
        self.wfile.write(html.encode())

    def send_json(self, data: dict, status: int = 200):
        body = json.dumps(data)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body.encode())

    def render_page(self, message: str = "", message_type: str = ""):
        wifi = get_wifi_status()
        cell = get_cellular_status()

        # Format message
        msg_html = ""
        if message:
            msg_html = f'<div class="message {message_type}">{message}</div>'

        # Format available networks
        networks_html = "<p style='color:#888;padding:10px;'>Click 'Scan' to find networks</p>"

        html = HTML_TEMPLATE.format(
            message=msg_html,
            wifi_status_class="connected" if wifi["connected"] else "disconnected",
            wifi_status_text="Connected" if wifi["connected"] else "Disconnected",
            wifi_ssid=wifi["ssid"] or "-",
            wifi_ip=wifi["ip"] or "-",
            wifi_signal=wifi["signal"] or "-",
            cell_status_class="connected" if cell["connected"] else "disconnected",
            cell_status_text="Connected" if cell["connected"] else "Disconnected",
            cell_operator=cell["operator"] or "-",
            cell_technology=cell["technology"] or "-",
            cell_ip=cell["ip"] or "-",
            cell_signal=cell["signal"] or "-",
            available_networks=networks_html
        )
        return html

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            self.send_html(self.render_page())

        elif path == "/wifi/scan":
            networks = scan_wifi_networks()
            networks_html = ""
            for net in networks:
                networks_html += f'''<div class="network-item" data-ssid="{net["ssid"]}">
                    <div class="ssid">{net["ssid"]}</div>
                    <div class="meta">{net["signal"]} dBm | {net["security"]}</div>
                </div>'''
            if not networks_html:
                networks_html = "<p style='color:#888;padding:10px;'>No networks found</p>"

            wifi = get_wifi_status()
            cell = get_cellular_status()
            html = HTML_TEMPLATE.format(
                message='<div class="message success">Scan complete</div>',
                wifi_status_class="connected" if wifi["connected"] else "disconnected",
                wifi_status_text="Connected" if wifi["connected"] else "Disconnected",
                wifi_ssid=wifi["ssid"] or "-",
                wifi_ip=wifi["ip"] or "-",
                wifi_signal=wifi["signal"] or "-",
                cell_status_class="connected" if cell["connected"] else "disconnected",
                cell_status_text="Connected" if cell["connected"] else "Disconnected",
                cell_operator=cell["operator"] or "-",
                cell_technology=cell["technology"] or "-",
                cell_ip=cell["ip"] or "-",
                cell_signal=cell["signal"] or "-",
                available_networks=networks_html
            )
            self.send_html(html)

        elif path == "/api/status":
            self.send_json({
                "wifi": get_wifi_status(),
                "cellular": get_cellular_status()
            })

        else:
            self.send_html(self.render_page(), 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length).decode('utf-8')
        params = parse_qs(body)

        if path == "/wifi/connect":
            ssid = params.get("ssid", [""])[0]
            password = params.get("password", [""])[0]

            success, message = add_wifi_network(ssid, password)
            msg_type = "success" if success else "error"
            self.send_html(self.render_page(message, msg_type))

        else:
            self.send_html(self.render_page("Not found", "error"), 404)


def main():
    print(f"Starting web interface on port {PORT}...")
    server = HTTPServer(("0.0.0.0", PORT), RequestHandler)
    print(f"Server running at http://0.0.0.0:{PORT}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.shutdown()


if __name__ == "__main__":
    main()
