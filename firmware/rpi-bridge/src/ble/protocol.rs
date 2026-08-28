//! BLE Protocol Message Definitions
//!
//! Defines the JSON message format for communication between the Flutter app
//! and the BLE bridge. The bridge proxies HTTP requests to the Java backend.

use serde::{Deserialize, Serialize};

/// Request from phone app to bridge
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum AppRequest {
    /// HTTP request to proxy to Java backend
    #[serde(rename = "http")]
    Http(HttpRequest),

    /// Request to download a file (for large files like videos)
    #[serde(rename = "file_download")]
    FileDownload(FileDownloadRequest),

    /// Ping to check connection
    #[serde(rename = "ping")]
    Ping { id: u32 },

    /// Request device info
    #[serde(rename = "info")]
    Info { id: u32 },
}

/// HTTP request to proxy
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HttpRequest {
    /// Request ID for matching responses
    pub id: u32,
    /// HTTP method (GET, POST, PUT, DELETE, etc.)
    pub method: String,
    /// Request path (e.g., "/getSeedSummaryData")
    pub path: String,
    /// Optional request body (for POST/PUT)
    #[serde(default)]
    pub body: Option<String>,
    /// Optional headers
    #[serde(default)]
    pub headers: Option<std::collections::HashMap<String, String>>,
}

/// Request to download a large file
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileDownloadRequest {
    /// Request ID for matching responses
    pub id: u32,
    /// File path on the server
    pub path: String,
}

/// Response from bridge to phone app
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum BridgeResponse {
    /// HTTP response from Java backend
    #[serde(rename = "http")]
    Http(HttpResponse),

    /// File download metadata (sent before chunks)
    #[serde(rename = "file_meta")]
    FileMeta(FileMetaResponse),

    /// File chunk (for large file transfers)
    #[serde(rename = "file_chunk")]
    FileChunk(FileChunk),

    /// Pong response
    #[serde(rename = "pong")]
    Pong { id: u32 },

    /// Device info response
    #[serde(rename = "info")]
    Info(InfoResponse),

    /// Error response
    #[serde(rename = "error")]
    Error(ErrorResponse),
}

/// HTTP response from backend
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HttpResponse {
    /// Request ID this responds to
    pub id: u32,
    /// HTTP status code
    pub status: u16,
    /// Response body (JSON string or text)
    pub body: String,
    /// Content type
    #[serde(default)]
    pub content_type: Option<String>,
}

/// File download metadata
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileMetaResponse {
    /// Request ID this responds to
    pub id: u32,
    /// Total file size in bytes
    pub size: u64,
    /// Total number of chunks
    pub total_chunks: u32,
    /// Content type (e.g., "video/mp4", "image/png")
    pub content_type: String,
    /// Filename
    pub filename: String,
}

/// File chunk for large file transfers
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileChunk {
    /// Request ID this belongs to
    pub id: u32,
    /// Chunk index (0-based)
    pub chunk_index: u32,
    /// Total number of chunks
    pub total_chunks: u32,
    /// Base64-encoded chunk data
    pub data: String,
    /// Whether this is the last chunk
    pub is_last: bool,
}

/// Device info response
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InfoResponse {
    /// Request ID
    pub id: u32,
    /// Bridge version
    pub version: String,
    /// Device name
    pub device_name: String,
    /// Java backend URL
    pub backend_url: String,
    /// Whether backend is reachable
    pub backend_online: bool,
}

/// Error response
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ErrorResponse {
    /// Request ID this responds to
    pub id: u32,
    /// Error code
    pub code: String,
    /// Human-readable error message
    pub message: String,
}

impl BridgeResponse {
    /// Create an error response
    pub fn error(id: u32, code: &str, message: &str) -> Self {
        BridgeResponse::Error(ErrorResponse {
            id,
            code: code.to_string(),
            message: message.to_string(),
        })
    }

    /// Create an HTTP error response
    pub fn http_error(id: u32, status: u16, message: &str) -> Self {
        BridgeResponse::Http(HttpResponse {
            id,
            status,
            body: serde_json::json!({ "error": message }).to_string(),
            content_type: Some("application/json".to_string()),
        })
    }
}
