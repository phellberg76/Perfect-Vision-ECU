//! ESP32 State Management
//!
//! Caches GPS and WiFi state received from ESP32 via UART.
//! Provides methods to query state and send commands.

use serde::{Deserialize, Serialize};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

/// GPS data from ESP32
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GpsState {
    pub lat: Option<f64>,
    pub lng: Option<f64>,
    pub alt: Option<f64>,
    pub speed: Option<f64>,
    pub course: Option<f64>,
    pub sats: i32,
    pub hdop: Option<f64>,
    pub fix: bool,
    pub time: Option<String>,
    #[serde(skip)]
    pub last_update: Option<Instant>,
}

/// WiFi data from ESP32
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct WifiState {
    pub connected: bool,
    pub ssid: Option<String>,
    pub ip: Option<String>,
    pub rssi: Option<i32>,
    pub configured: bool,
    #[serde(skip)]
    pub last_update: Option<Instant>,
}

/// Combined ESP32 state
#[derive(Debug, Default)]
pub struct Esp32State {
    pub gps: GpsState,
    pub wifi: WifiState,
}

/// Thread-safe ESP32 state wrapper
#[derive(Clone)]
pub struct SharedEsp32State {
    inner: Arc<RwLock<Esp32State>>,
}

impl SharedEsp32State {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(RwLock::new(Esp32State::default())),
        }
    }

    /// Update GPS state from ESP32 message
    pub fn update_gps(&self, gps: GpsState) {
        if let Ok(mut state) = self.inner.write() {
            state.gps = GpsState {
                last_update: Some(Instant::now()),
                ..gps
            };
        }
    }

    /// Update WiFi state from ESP32 message
    pub fn update_wifi(&self, wifi: WifiState) {
        if let Ok(mut state) = self.inner.write() {
            state.wifi = WifiState {
                last_update: Some(Instant::now()),
                ..wifi
            };
        }
    }

    /// Get current GPS state
    pub fn get_gps(&self) -> Option<GpsState> {
        self.inner.read().ok().map(|s| s.gps.clone())
    }

    /// Get current WiFi state
    pub fn get_wifi(&self) -> Option<WifiState> {
        self.inner.read().ok().map(|s| s.wifi.clone())
    }

    /// Check if GPS data is fresh (within given duration)
    pub fn is_gps_fresh(&self, max_age: Duration) -> bool {
        self.inner
            .read()
            .ok()
            .and_then(|s| s.gps.last_update)
            .map(|t| t.elapsed() < max_age)
            .unwrap_or(false)
    }

    /// Check if WiFi data is fresh
    pub fn is_wifi_fresh(&self, max_age: Duration) -> bool {
        self.inner
            .read()
            .ok()
            .and_then(|s| s.wifi.last_update)
            .map(|t| t.elapsed() < max_age)
            .unwrap_or(false)
    }
}

impl Default for SharedEsp32State {
    fn default() -> Self {
        Self::new()
    }
}

/// Commands that can be sent to ESP32
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "cmd")]
pub enum Esp32Command {
    #[serde(rename = "wifi_status")]
    WifiStatus,

    #[serde(rename = "wifi_config")]
    WifiConfig { ssid: String, password: String },

    #[serde(rename = "wifi_disconnect")]
    WifiDisconnect,

    #[serde(rename = "gps_status")]
    GpsStatus,
}

/// Parse incoming JSON message from ESP32
#[derive(Debug, Deserialize)]
#[serde(tag = "type")]
pub enum Esp32Message {
    #[serde(rename = "gps")]
    Gps(GpsState),

    #[serde(rename = "wifi")]
    Wifi(WifiState),

    #[serde(rename = "wifi_config_ack")]
    WifiConfigAck { status: String, msg: Option<String> },

    #[serde(rename = "heartbeat")]
    Heartbeat {
        uptime: u64,
        gps_fix: bool,
        sats: Option<i32>,
        ble_connected: bool,
        wifi_connected: Option<bool>,
    },

    #[serde(rename = "boot")]
    Boot { msg: String },

    #[serde(rename = "init")]
    Init { step: String },

    #[serde(rename = "startup")]
    Startup {
        device: String,
        version: String,
        mode: String,
    },

    #[serde(rename = "ready")]
    Ready { ble: bool },

    #[serde(rename = "error")]
    Error { msg: String },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_gps_message() {
        let json = r#"{"type":"gps","lat":59.123,"lng":18.456,"fix":true,"sats":8}"#;
        let msg: Esp32Message = serde_json::from_str(json).unwrap();
        match msg {
            Esp32Message::Gps(gps) => {
                assert!(gps.fix);
                assert_eq!(gps.sats, 8);
            }
            _ => panic!("Expected GPS message"),
        }
    }

    #[test]
    fn test_parse_wifi_message() {
        let json = r#"{"type":"wifi","connected":true,"ssid":"MyNetwork","ip":"192.168.1.100","rssi":-45,"configured":true}"#;
        let msg: Esp32Message = serde_json::from_str(json).unwrap();
        match msg {
            Esp32Message::Wifi(wifi) => {
                assert!(wifi.connected);
                assert_eq!(wifi.ssid, Some("MyNetwork".to_string()));
            }
            _ => panic!("Expected WiFi message"),
        }
    }

    #[test]
    fn test_serialize_wifi_config_command() {
        let cmd = Esp32Command::WifiConfig {
            ssid: "TestNetwork".to_string(),
            password: "secret123".to_string(),
        };
        let json = serde_json::to_string(&cmd).unwrap();
        assert!(json.contains("wifi_config"));
        assert!(json.contains("TestNetwork"));
    }
}
