//! Planter BLE Bridge
//!
//! Bridge for the Planter App to communicate with the Java backend.
//!
//! Supports two modes:
//!   - UART mode: ESP32 handles BLE, this bridge handles HTTP proxy
//!   - BLE mode: This bridge handles both BLE GATT server and HTTP proxy (legacy)
//!
//! Architecture (UART mode - recommended):
//!   Flutter App <--BLE--> ESP32 <--UART--> This Bridge <--HTTP--> Java Backend
//!
//! Architecture (BLE mode - legacy):
//!   Flutter App <--BLE GATT--> This Bridge <--HTTP--> Java Backend

mod ble;
mod http_proxy;
mod uart_bridge;

use ble::{spawn_gatt_server, AppRequest, BridgeResponse, GattCommand, GattEvent};
use clap::Parser;
use http_proxy::HttpProxy;
use std::sync::mpsc;
use uart_bridge::{spawn_uart_bridge, UartCommand, UartEvent};

/// Planter BLE Bridge - BLE/UART to HTTP proxy for Planter App
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Bluetooth device name (only used in BLE mode)
    #[arg(short, long, default_value = "PerfectPrecisionPlanter")]
    name: String,

    /// Java backend URL
    #[arg(short, long, default_value = "http://localhost:8080")]
    backend: String,

    /// Use UART mode instead of BLE (ESP32 handles BLE)
    #[arg(short, long)]
    uart: bool,

    /// UART port (only used in UART mode)
    #[arg(short, long, default_value = "/dev/ttyAMA0")]
    port: String,

    /// UART baud rate
    #[arg(long, default_value = "115200")]
    baud: u32,

    /// Enable debug logging
    #[arg(short, long)]
    debug: bool,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // Initialize logging
    let log_level = if args.debug { "debug" } else { "info" };
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(log_level)).init();

    log::info!("Planter BLE Bridge v{}", env!("CARGO_PKG_VERSION"));
    log::info!("Backend URL: {}", args.backend);

    // Create HTTP proxy
    let proxy = HttpProxy::new(&args.backend, &args.name)?;

    // Check backend health
    if proxy.check_health().await {
        log::info!("Backend is online");
    } else {
        log::warn!("Backend is not reachable - will retry on requests");
    }

    if args.uart {
        run_uart_mode(args, proxy).await
    } else {
        run_ble_mode(args, proxy).await
    }
}

async fn run_uart_mode(
    args: Args,
    proxy: HttpProxy,
) -> Result<(), Box<dyn std::error::Error>> {
    log::info!("Running in UART mode");
    log::info!("UART port: {} @ {} baud", args.port, args.baud);

    // Create channels for UART communication
    let (event_tx, event_rx) = mpsc::channel::<UartEvent>();
    let (command_tx, command_rx) = tokio::sync::mpsc::channel::<UartCommand>(100);

    // Spawn UART bridge
    let _uart_thread = spawn_uart_bridge(args.port.clone(), args.baud, event_tx, command_rx);

    log::info!("UART bridge started, waiting for messages from ESP32...");

    // Main event loop
    loop {
        match event_rx.try_recv() {
            Ok(event) => {
                match event {
                    UartEvent::Started { port } => {
                        log::info!("UART ready: {}", port);
                    }
                    UartEvent::MessageReceived(message) => {
                        log::debug!("Received: {}", message);

                        // Parse request
                        match serde_json::from_str::<AppRequest>(&message) {
                            Ok(request) => {
                                // Handle request
                                let responses = proxy.handle_request(request).await;

                                // Send responses back via UART
                                for response in responses {
                                    match serde_json::to_string(&response) {
                                        Ok(json) => {
                                            if let Err(e) =
                                                command_tx.send(UartCommand::Send(json)).await
                                            {
                                                log::error!("Failed to send response: {}", e);
                                            }
                                        }
                                        Err(e) => {
                                            log::error!("Failed to serialize response: {}", e);
                                        }
                                    }
                                }
                            }
                            Err(e) => {
                                log::error!("Failed to parse request: {}", e);
                                log::debug!("Raw message: {}", message);

                                // Send error response
                                let error =
                                    BridgeResponse::error(0, "parse_error", &e.to_string());
                                if let Ok(json) = serde_json::to_string(&error) {
                                    let _ = command_tx.send(UartCommand::Send(json)).await;
                                }
                            }
                        }
                    }
                    UartEvent::Error(e) => {
                        log::error!("UART error: {}", e);
                    }
                }
            }
            Err(mpsc::TryRecvError::Empty) => {
                tokio::time::sleep(tokio::time::Duration::from_millis(10)).await;
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                log::error!("UART event channel disconnected");
                break;
            }
        }
    }

    log::info!("Shutting down...");
    let _ = command_tx.send(UartCommand::Shutdown).await;

    Ok(())
}

async fn run_ble_mode(
    args: Args,
    proxy: HttpProxy,
) -> Result<(), Box<dyn std::error::Error>> {
    log::info!("Running in BLE mode (legacy)");
    log::info!("Device name: {}", args.name);

    // Create channels for GATT server communication
    let (event_tx, event_rx) = mpsc::channel::<GattEvent>();
    let (command_tx, command_rx) = tokio::sync::mpsc::channel::<GattCommand>(100);

    // Spawn GATT server in dedicated thread
    let device_name = args.name.clone();
    let _gatt_thread = spawn_gatt_server(device_name, event_tx, command_rx);

    log::info!("GATT server started, waiting for connections...");

    // Main event loop
    loop {
        match event_rx.try_recv() {
            Ok(event) => {
                match event {
                    GattEvent::Started { adapter_address } => {
                        log::info!("BLE adapter ready: {}", adapter_address);
                    }
                    GattEvent::Connected => {
                        log::info!("App connected");
                    }
                    GattEvent::Disconnected => {
                        log::info!("App disconnected");
                    }
                    GattEvent::MessageReceived(message) => {
                        log::debug!("Received: {}", message);

                        // Parse request
                        match serde_json::from_str::<AppRequest>(&message) {
                            Ok(request) => {
                                // Handle request
                                let responses = proxy.handle_request(request).await;

                                // Send responses
                                for response in responses {
                                    match serde_json::to_string(&response) {
                                        Ok(json) => {
                                            if let Err(e) =
                                                command_tx.send(GattCommand::Send(json)).await
                                            {
                                                log::error!("Failed to send response: {}", e);
                                            }
                                        }
                                        Err(e) => {
                                            log::error!("Failed to serialize response: {}", e);
                                        }
                                    }
                                }
                            }
                            Err(e) => {
                                log::error!("Failed to parse request: {}", e);
                                log::debug!("Raw message: {}", message);

                                // Send error response
                                let error =
                                    BridgeResponse::error(0, "parse_error", &e.to_string());
                                if let Ok(json) = serde_json::to_string(&error) {
                                    let _ = command_tx.send(GattCommand::Send(json)).await;
                                }
                            }
                        }
                    }
                    GattEvent::Error(e) => {
                        log::error!("GATT error: {}", e);
                    }
                }
            }
            Err(mpsc::TryRecvError::Empty) => {
                tokio::time::sleep(tokio::time::Duration::from_millis(10)).await;
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                log::error!("GATT event channel disconnected");
                break;
            }
        }
    }

    log::info!("Shutting down...");
    let _ = command_tx.send(GattCommand::Shutdown).await;

    Ok(())
}
