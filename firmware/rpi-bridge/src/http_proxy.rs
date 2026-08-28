//! HTTP Proxy for Java Backend
//!
//! Forwards HTTP requests to the Java planter app running on localhost.

use crate::ble::protocol::{
    AppRequest, BridgeResponse, FileChunk, FileMetaResponse, HttpRequest, HttpResponse,
    InfoResponse,
};
use reqwest::Client;
use std::path::Path;
use std::time::Duration;

/// HTTP client configuration
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const FILE_CHUNK_SIZE: usize = 32 * 1024; // 32KB chunks for file transfers

/// HTTP Proxy to Java backend
pub struct HttpProxy {
    client: Client,
    base_url: String,
    device_name: String,
}

impl HttpProxy {
    /// Create a new HTTP proxy
    pub fn new(base_url: &str, device_name: &str) -> Result<Self, reqwest::Error> {
        let client = Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()?;

        Ok(Self {
            client,
            base_url: base_url.trim_end_matches('/').to_string(),
            device_name: device_name.to_string(),
        })
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
}
