//! HTTP Proxy for Java Backend
//!
//! Forwards HTTP requests to the Java planter app running on localhost.
//! Intercepts WiFi requests to handle them directly on the RPi via nmcli.
//! Intercepts GPS requests to return cached data from ESP32.

use crate::ble::protocol::{
    AppRequest, BridgeResponse, FileChunk, FileMetaResponse, HttpRequest, HttpResponse,
    InfoResponse,
};
use crate::esp32_state::{Esp32Command, SharedEsp32State};
use crate::uart_bridge::UartCommand;
use reqwest::Client;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};

/// HTTP client configuration
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const FILE_CHUNK_SIZE: usize = 32 * 1024; // 32KB chunks for file transfers

/// HTTP Proxy to Java backend
pub struct HttpProxy {
    client: Client,
    base_url: String,
    device_name: String,
    esp32_state: SharedEsp32State,
    uart_tx: Arc<RwLock<Option<mpsc::Sender<UartCommand>>>>,
}

impl HttpProxy {
    /// Create a new HTTP proxy
    pub fn new(
        base_url: &str,
        device_name: &str,
        esp32_state: SharedEsp32State,
    ) -> Result<Self, reqwest::Error> {
        let client = Client::builder().timeout(REQUEST_TIMEOUT).build()?;

        Ok(Self {
            client,
            base_url: base_url.trim_end_matches('/').to_string(),
            device_name: device_name.to_string(),
            esp32_state,
            uart_tx: Arc::new(RwLock::new(None)),
        })
    }

    /// Get the ESP32 state
    pub fn esp32_state(&self) -> SharedEsp32State {
        self.esp32_state.clone()
    }

    /// Set the UART command sender for sending commands to ESP32
    pub async fn set_uart_tx(&self, tx: mpsc::Sender<UartCommand>) {
        let mut guard = self.uart_tx.write().await;
        *guard = Some(tx);
    }

    /// Send a command to ESP32 via UART
    async fn send_esp32_command(&self, cmd: Esp32Command) -> bool {
        let guard = self.uart_tx.read().await;
        if let Some(tx) = guard.as_ref() {
            match serde_json::to_string(&cmd) {
                Ok(json) => {
                    if let Err(e) = tx.send(UartCommand::Send(json)).await {
                        log::error!("Failed to send ESP32 command: {}", e);
                        return false;
                    }
                    true
                }
                Err(e) => {
                    log::error!("Failed to serialize ESP32 command: {}", e);
                    false
                }
            }
        } else {
            log::warn!("UART not available for ESP32 commands");
            false
        }
    }

    /// Check if backend is reachable
    pub async fn check_health(&self) -> bool {
        let url = format!("{}/", self.base_url);
        match self.client.get(&url).send().await {
            Ok(resp) => resp.status().is_success() || resp.status().as_u16() == 404,
            Err(_) => false,
        }
    }

    /// Handle an incoming request from the app
    pub async fn handle_request(&self, request: AppRequest) -> Vec<BridgeResponse> {
        match request {
            AppRequest::Http(req) => vec![self.handle_http_request(req).await],
            AppRequest::FileDownload(req) => self.handle_file_download(req.id, &req.path).await,
            AppRequest::Ping { id } => vec![BridgeResponse::Pong { id }],
            AppRequest::Info { id } => vec![self.handle_info_request(id).await],
        }
    }

    /// Handle an HTTP request
    async fn handle_http_request(&self, req: HttpRequest) -> BridgeResponse {
        // Check for interceptable requests (WiFi/GPS via ESP32)
        if let Some(response) = self.try_intercept_request(&req).await {
            return response;
        }

        let url = format!("{}{}", self.base_url, req.path);
        log::info!("[{}] {} {}", req.id, req.method, url);

        let method = match req.method.to_uppercase().as_str() {
            "GET" => reqwest::Method::GET,
            "POST" => reqwest::Method::POST,
            "PUT" => reqwest::Method::PUT,
            "DELETE" => reqwest::Method::DELETE,
            "PATCH" => reqwest::Method::PATCH,
            "HEAD" => reqwest::Method::HEAD,
            "OPTIONS" => reqwest::Method::OPTIONS,
            _ => {
                return BridgeResponse::http_error(req.id, 400, "Invalid HTTP method");
            }
        };

        // Build request
        let mut request_builder = self.client.request(method, &url);

        // Add headers
        if let Some(headers) = req.headers {
            for (key, value) in headers {
                request_builder = request_builder.header(&key, &value);
            }
        }

        // Add body for POST/PUT/PATCH
        if let Some(body) = req.body {
            request_builder = request_builder.body(body);
        }

        // Send request
        match request_builder.send().await {
            Ok(response) => {
                let status = response.status().as_u16();
                let content_type = response
                    .headers()
                    .get("content-type")
                    .and_then(|v| v.to_str().ok())
                    .map(|s| s.to_string());

                match response.text().await {
                    Ok(body) => {
                        log::info!("[{}] Response: {} ({} bytes)", req.id, status, body.len());
                        BridgeResponse::Http(HttpResponse {
                            id: req.id,
                            status,
                            body,
                            content_type,
                        })
                    }
                    Err(e) => {
                        log::error!("[{}] Failed to read response body: {}", req.id, e);
                        BridgeResponse::http_error(req.id, 502, &format!("Failed to read response: {}", e))
                    }
                }
            }
            Err(e) => {
                log::error!("[{}] Request failed: {}", req.id, e);
                if e.is_timeout() {
                    BridgeResponse::http_error(req.id, 504, "Request timeout")
                } else if e.is_connect() {
                    BridgeResponse::http_error(req.id, 503, "Backend unavailable")
                } else {
                    BridgeResponse::http_error(req.id, 502, &format!("Request failed: {}", e))
                }
            }
        }
    }

    /// Handle a file download request (returns metadata + chunks)
    async fn handle_file_download(&self, id: u32, path: &str) -> Vec<BridgeResponse> {
        let url = format!("{}{}", self.base_url, path);
        log::info!("[{}] File download: {}", id, url);

        match self.client.get(&url).send().await {
            Ok(response) => {
                if !response.status().is_success() {
                    return vec![BridgeResponse::http_error(
                        id,
                        response.status().as_u16(),
                        "File not found",
                    )];
                }

                let content_type = response
                    .headers()
                    .get("content-type")
                    .and_then(|v| v.to_str().ok())
                    .unwrap_or("application/octet-stream")
                    .to_string();

                let _content_length = response
                    .headers()
                    .get("content-length")
                    .and_then(|v| v.to_str().ok())
                    .and_then(|s| s.parse::<u64>().ok())
                    .unwrap_or(0);

                // Extract filename from path
                let filename = Path::new(path)
                    .file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or("download")
                    .to_string();

                // Read entire file (for now - could stream in future)
                match response.bytes().await {
                    Ok(bytes) => {
                        let data = bytes.to_vec();
                        let size = data.len() as u64;
                        let total_chunks = ((data.len() + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE) as u32;

                        log::info!(
                            "[{}] File: {} bytes, {} chunks, type: {}",
                            id, size, total_chunks, content_type
                        );

                        let mut responses = Vec::with_capacity(total_chunks as usize + 1);

                        // Send metadata first
                        responses.push(BridgeResponse::FileMeta(FileMetaResponse {
                            id,
                            size,
                            total_chunks,
                            content_type,
                            filename,
                        }));

                        // Send chunks
                        let encoder = base64::engine::general_purpose::STANDARD;
                        use base64::Engine;

                        for (i, chunk) in data.chunks(FILE_CHUNK_SIZE).enumerate() {
                            let chunk_index = i as u32;
                            let is_last = chunk_index == total_chunks - 1;

                            responses.push(BridgeResponse::FileChunk(FileChunk {
                                id,
                                chunk_index,
                                total_chunks,
                                data: encoder.encode(chunk),
                                is_last,
                            }));
                        }

                        responses
                    }
                    Err(e) => {
                        log::error!("[{}] Failed to read file: {}", id, e);
                        vec![BridgeResponse::http_error(id, 502, &format!("Failed to read file: {}", e))]
                    }
                }
            }
            Err(e) => {
                log::error!("[{}] File download failed: {}", id, e);
                vec![BridgeResponse::http_error(id, 502, &format!("Download failed: {}", e))]
            }
        }
    }

    /// Handle info request
    async fn handle_info_request(&self, id: u32) -> BridgeResponse {
        let backend_online = self.check_health().await;

        BridgeResponse::Info(InfoResponse {
            id,
            version: env!("CARGO_PKG_VERSION").to_string(),
            device_name: self.device_name.clone(),
            backend_url: self.base_url.clone(),
            backend_online,
        })
    }

    // =========================================================================
    // ESP32 Request Interception (WiFi/GPS)
    // =========================================================================

    /// Try to intercept a request that should be handled by ESP32
    async fn try_intercept_request(&self, req: &HttpRequest) -> Option<BridgeResponse> {
        let path = req.path.as_str();

        // WiFi status request
        if path.contains("/getWifiStatus") {
            log::info!("[{}] Intercepting getWifiStatus", req.id);
            return Some(self.handle_wifi_status(req.id).await);
        }

        // WiFi scan request
        if path.contains("/scanWifi") {
            log::info!("[{}] Intercepting scanWifi", req.id);
            return Some(self.handle_wifi_scan(req.id).await);
        }

        // WiFi configuration request
        if path.contains("/configureWifi") {
            log::info!("[{}] Intercepting configureWifi", req.id);
            return Some(self.handle_wifi_configure(req.id, path).await);
        }

        // GPS status request (optional - could also let Java handle this)
        if path.contains("/getSpiGpsStatus") {
            log::info!("[{}] Intercepting getSpiGpsStatus", req.id);
            return Some(self.handle_gps_status(req.id).await);
        }

        // Watchdog pet request - forward to ESP32
        if path.contains("/petWatchdog") {
            log::debug!("[{}] Intercepting petWatchdog", req.id);
            return Some(self.handle_watchdog_pet(req.id).await);
        }

        None
    }

    /// Handle watchdog pet request - forward to ESP32
    async fn handle_watchdog_pet(&self, id: u32) -> BridgeResponse {
        // Send wdt_pet command to ESP32 via UART
        let guard = self.uart_tx.read().await;
        if let Some(tx) = guard.as_ref() {
            let cmd = r#"{"cmd":"wdt_pet"}"#.to_string();
            match tx.send(UartCommand::Send(cmd)).await {
                Ok(_) => {
                    log::trace!("Watchdog pet forwarded to ESP32");
                    BridgeResponse::Http(HttpResponse {
                        id,
                        status: 200,
                        body: r#"{"status":"ok"}"#.to_string(),
                        content_type: Some("application/json".to_string()),
                    })
                }
                Err(e) => {
                    log::error!("Failed to forward watchdog pet: {}", e);
                    BridgeResponse::Http(HttpResponse {
                        id,
                        status: 503,
                        body: r#"{"error":"Failed to send to ESP32"}"#.to_string(),
                        content_type: Some("application/json".to_string()),
                    })
                }
            }
        } else {
            log::warn!("UART not available for watchdog pet");
            BridgeResponse::Http(HttpResponse {
                id,
                status: 503,
                body: r#"{"error":"UART not available"}"#.to_string(),
                content_type: Some("application/json".to_string()),
            })
        }

    /// Handle WiFi status request - returns RPi's WiFi status
    async fn handle_wifi_status(&self, id: u32) -> BridgeResponse {
        let wifi_status = self.get_rpi_wifi_status().await;
        BridgeResponse::Http(HttpResponse {
            id,
            status: 200,
            body: wifi_status,
            content_type: Some("application/json".to_string()),
        })
    }

    /// Get RPi WiFi status using nmcli
    async fn get_rpi_wifi_status(&self) -> String {
        // Get active connection info
        let connection = tokio::process::Command::new("nmcli")
            .args(["-t", "-f", "NAME,TYPE,DEVICE", "connection", "show", "--active"])
            .output()
            .await;

        let mut connected = false;
        let mut ssid: Option<String> = None;
        let mut device: Option<String> = None;

        if let Ok(output) = connection {
            let stdout = String::from_utf8_lossy(&output.stdout);
            for line in stdout.lines() {
                let parts: Vec<&str> = line.split(':').collect();
                // Format: NAME:TYPE:DEVICE
                if parts.len() >= 3 && parts[1] == "802-11-wireless" {
                    connected = true;
                    ssid = Some(parts[0].to_string());
                    device = Some(parts[2].to_string());
                    break;
                }
            }
        }

        if !connected {
            return r#"{"connected":false}"#.to_string();
        }

        let mut json = format!(r#"{{"connected":true"#);

        if let Some(ref s) = ssid {
            json.push_str(&format!(r#","ssid":"{}""#, s));
        }

        // Get IP address
        if let Some(ref dev) = device {
            if let Ok(output) = tokio::process::Command::new("nmcli")
                .args(["-t", "-f", "IP4.ADDRESS", "device", "show", dev])
                .output()
                .await
            {
                let stdout = String::from_utf8_lossy(&output.stdout);
                for line in stdout.lines() {
                    if line.starts_with("IP4.ADDRESS") {
                        if let Some(addr) = line.split(':').nth(1) {
                            // Remove CIDR notation (e.g., /24)
                            let ip = addr.split('/').next().unwrap_or(addr);
                            json.push_str(&format!(r#","ip":"{}""#, ip));
                            break;
                        }
                    }
                }
            }

            // Get signal strength
            if let Ok(output) = tokio::process::Command::new("nmcli")
                .args(["-t", "-f", "IN-USE,SIGNAL", "dev", "wifi", "list"])
                .output()
                .await
            {
                let stdout = String::from_utf8_lossy(&output.stdout);
                for line in stdout.lines() {
                    if line.starts_with('*') {
                        // Active connection marked with *
                        if let Some(signal) = line.split(':').nth(1) {
                            if let Ok(pct) = signal.parse::<i32>() {
                                // Convert percentage to approximate dBm
                                // 100% ≈ -30dBm, 0% ≈ -90dBm
                                let rssi = -90 + (pct * 60 / 100);
                                json.push_str(&format!(r#","rssi":{}"#, rssi));
                            }
                            break;
                        }
                    }
                }
            }
        }

        json.push('}');
        json
    }

    /// Handle WiFi scan request - returns list of available networks
    async fn handle_wifi_scan(&self, id: u32) -> BridgeResponse {
        log::info!("Scanning for WiFi networks...");

        // Trigger a rescan first
        let _ = tokio::process::Command::new("nmcli")
            .args(["dev", "wifi", "rescan"])
            .output()
            .await;

        // Small delay to let scan complete
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Get list of networks
        // Format: IN-USE:SSID:SIGNAL:SECURITY
        let output = tokio::process::Command::new("nmcli")
            .args(["-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list"])
            .output()
            .await;

        match output {
            Ok(output) => {
                let stdout = String::from_utf8_lossy(&output.stdout);
                let mut networks: Vec<serde_json::Value> = Vec::new();
                let mut seen_ssids: std::collections::HashSet<String> = std::collections::HashSet::new();

                for line in stdout.lines() {
                    let parts: Vec<&str> = line.split(':').collect();
                    if parts.len() >= 4 {
                        let in_use = parts[0] == "*";
                        let ssid = parts[1].to_string();
                        let signal: i32 = parts[2].parse().unwrap_or(0);
                        let security = parts[3].to_string();

                        // Skip empty SSIDs and duplicates
                        if ssid.is_empty() || seen_ssids.contains(&ssid) {
                            continue;
                        }
                        seen_ssids.insert(ssid.clone());

                        // Convert signal percentage to dBm approximation
                        let rssi = -90 + (signal * 60 / 100);

                        networks.push(serde_json::json!({
                            "ssid": ssid,
                            "signal": signal,
                            "rssi": rssi,
                            "secured": !security.is_empty() && security != "--",
                            "inUse": in_use
                        }));
                    }
                }

                // Sort by signal strength (strongest first)
                networks.sort_by(|a, b| {
                    let sig_a = a["signal"].as_i64().unwrap_or(0);
                    let sig_b = b["signal"].as_i64().unwrap_or(0);
                    sig_b.cmp(&sig_a)
                });

                log::info!("Found {} WiFi networks", networks.len());

                let body = serde_json::json!({ "networks": networks }).to_string();
                BridgeResponse::Http(HttpResponse {
                    id,
                    status: 200,
                    body,
                    content_type: Some("application/json".to_string()),
                })
            }
            Err(e) => {
                log::error!("WiFi scan failed: {}", e);
                BridgeResponse::Http(HttpResponse {
                    id,
                    status: 500,
                    body: r#"{"error":"WiFi scan failed"}"#.to_string(),
                    content_type: Some("application/json".to_string()),
                })
            }
        }
    }

    /// Handle WiFi configure request - configures RPi WiFi using nmcli
    async fn handle_wifi_configure(&self, id: u32, path: &str) -> BridgeResponse {
        // Parse query parameters from path
        let (ssid, password) = self.parse_wifi_params(path);

        match ssid {
            Some(ssid) => {
                let password = password.unwrap_or_default();

                log::info!("Configuring RPi WiFi: ssid={}", ssid);

                // Try to connect using nmcli
                // First, try to delete any existing connection with this SSID
                let _ = tokio::process::Command::new("nmcli")
                    .args(["connection", "delete", &ssid])
                    .output()
                    .await;

                // Create new connection and connect
                let result = if password.is_empty() {
                    // Open network
                    tokio::process::Command::new("nmcli")
                        .args(["device", "wifi", "connect", &ssid])
                        .output()
                        .await
                } else {
                    // WPA/WPA2 network
                    tokio::process::Command::new("nmcli")
                        .args(["device", "wifi", "connect", &ssid, "password", &password])
                        .output()
                        .await
                };

                match result {
                    Ok(output) => {
                        if output.status.success() {
                            log::info!("WiFi connected to {}", ssid);
                            BridgeResponse::Http(HttpResponse {
                                id,
                                status: 200,
                                body: format!(r#"{{"status":"connected","ssid":"{}"}}"#, ssid),
                                content_type: Some("application/json".to_string()),
                            })
                        } else {
                            let stderr = String::from_utf8_lossy(&output.stderr);
                            log::error!("WiFi connection failed: {}", stderr);
                            BridgeResponse::Http(HttpResponse {
                                id,
                                status: 500,
                                body: format!(r#"{{"error":"Connection failed: {}"}}"#,
                                    stderr.lines().next().unwrap_or("Unknown error")),
                                content_type: Some("application/json".to_string()),
                            })
                        }
                    }
                    Err(e) => {
                        log::error!("Failed to run nmcli: {}", e);
                        BridgeResponse::Http(HttpResponse {
                            id,
                            status: 503,
                            body: r#"{"error":"Failed to configure WiFi"}"#.to_string(),
                            content_type: Some("application/json".to_string()),
                        })
                    }
                }
            }
            None => BridgeResponse::Http(HttpResponse {
                id,
                status: 400,
                body: r#"{"error":"SSID is required"}"#.to_string(),
                content_type: Some("application/json".to_string()),
            }),
        }
    }

    /// Parse WiFi parameters from URL path
    fn parse_wifi_params(&self, path: &str) -> (Option<String>, Option<String>) {
        let mut ssid = None;
        let mut password = None;

        if let Some(query_start) = path.find('?') {
            let query = &path[query_start + 1..];
            for param in query.split('&') {
                if let Some((key, value)) = param.split_once('=') {
                    let decoded = urlencoding::decode(value).unwrap_or_default();
                    match key {
                        "ssid" => ssid = Some(decoded.to_string()),
                        "password" => password = Some(decoded.to_string()),
                        _ => {}
                    }
                }
            }
        }

        (ssid, password)
    }

    /// Handle GPS status request
    async fn handle_gps_status(&self, id: u32) -> BridgeResponse {
        // Check if we have fresh GPS data
        if !self.esp32_state.is_gps_fresh(Duration::from_secs(5)) {
            // Request fresh data from ESP32
            self.send_esp32_command(Esp32Command::GpsStatus).await;

            // Wait a bit for the response
            tokio::time::sleep(Duration::from_millis(200)).await;
        }

        // Get cached GPS state
        match self.esp32_state.get_gps() {
            Some(gps) => {
                let json = serde_json::to_string(&gps).unwrap_or_else(|_| "{}".to_string());
                BridgeResponse::Http(HttpResponse {
                    id,
                    status: 200,
                    body: json,
                    content_type: Some("application/json".to_string()),
                })
            }
            None => BridgeResponse::Http(HttpResponse {
                id,
                status: 200,
                body: r#"{"fix":false,"sats":0}"#.to_string(),
                content_type: Some("application/json".to_string()),
            }),
        }
    }
}
