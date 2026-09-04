//! HTTP Proxy for Java Backend
//!
//! Forwards HTTP requests to the Java planter app running on localhost.
//! Intercepts WiFi and GPS requests to handle them directly via ESP32.

use crate::ble::protocol::{
    AppRequest, BridgeResponse, FileChunk, FileMetaResponse, HttpRequest, HttpResponse,
    InfoResponse,
};
use crate::esp32_state::{Esp32Command, SharedEsp32State, WifiState};
use crate::uart_bridge::UartCommand;
use reqwest::Client;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};

/// HTTP client configuration
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const FILE_CHUNK_SIZE: usize = 32 * 1024; // 32KB chunks for file transfers

/// How fresh WiFi data needs to be before we request an update
const WIFI_DATA_MAX_AGE: Duration = Duration::from_secs(10);

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
    pub fn set_uart_tx(&self, tx: mpsc::Sender<UartCommand>) {
        // Use blocking lock since this is called once at startup
        let rt = tokio::runtime::Handle::current();
        rt.block_on(async {
            let mut guard = self.uart_tx.write().await;
            *guard = Some(tx);
        });
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

        None
    }

    /// Handle WiFi status request
    async fn handle_wifi_status(&self, id: u32) -> BridgeResponse {
        // Check if we have fresh WiFi data
        if !self.esp32_state.is_wifi_fresh(WIFI_DATA_MAX_AGE) {
            // Request fresh data from ESP32
            self.send_esp32_command(Esp32Command::WifiStatus).await;

            // Wait a bit for the response
            tokio::time::sleep(Duration::from_millis(200)).await;
        }

        // Get cached WiFi state
        match self.esp32_state.get_wifi() {
            Some(wifi) => {
                let json = self.wifi_state_to_json(&wifi);
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
                body: r#"{"connected":false,"configured":false}"#.to_string(),
                content_type: Some("application/json".to_string()),
            }),
        }
    }

    /// Convert WiFi state to JSON response (matching Java endpoint format)
    fn wifi_state_to_json(&self, wifi: &WifiState) -> String {
        let mut json = format!(r#"{{"connected":{}"#, wifi.connected);

        if let Some(ref ssid) = wifi.ssid {
            json.push_str(&format!(r#","ssid":"{}""#, ssid));
        }

        if let Some(ref ip) = wifi.ip {
            json.push_str(&format!(r#","ip":"{}""#, ip));
        }

        if let Some(rssi) = wifi.rssi {
            json.push_str(&format!(r#","rssi":{}"#, rssi));
        }

        json.push('}');
        json
    }

    /// Handle WiFi configure request
    async fn handle_wifi_configure(&self, id: u32, path: &str) -> BridgeResponse {
        // Parse query parameters from path
        let (ssid, password) = self.parse_wifi_params(path);

        match ssid {
            Some(ssid) => {
                let password = password.unwrap_or_default();

                log::info!("Configuring WiFi: ssid={}", ssid);

                // Send configuration command to ESP32
                let success = self
                    .send_esp32_command(Esp32Command::WifiConfig {
                        ssid: ssid.clone(),
                        password,
                    })
                    .await;

                if success {
                    BridgeResponse::Http(HttpResponse {
                        id,
                        status: 200,
                        body: r#"{"status":"WiFi configuration sent"}"#.to_string(),
                        content_type: Some("application/json".to_string()),
                    })
                } else {
                    BridgeResponse::Http(HttpResponse {
                        id,
                        status: 503,
                        body: r#"{"error":"Failed to send WiFi configuration"}"#.to_string(),
                        content_type: Some("application/json".to_string()),
                    })
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
