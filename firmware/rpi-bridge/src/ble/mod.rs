//! BLE Communication Module
//!
//! Provides BLE GATT server functionality for communication with the Flutter app.

pub mod gatt_server;
pub mod protocol;

pub use gatt_server::{spawn_gatt_server, GattCommand, GattEvent};
pub use protocol::{AppRequest, BridgeResponse};
