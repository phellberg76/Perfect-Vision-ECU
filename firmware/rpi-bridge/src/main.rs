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
//!
//! ESP32 State:
//!   The bridge caches GPS and WiFi state from ESP32 and handles these requests
//!   directly without proxying to the Java backend.

mod ble;
mod esp32_state;
mod http_proxy;
mod uart_bridge;

use ble::{spawn_gatt_server, AppRequest, BridgeResponse, GattCommand, GattEvent};
use clap::Parser;
use esp32_state::{Esp32Message, SharedEsp32State};
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

    // Create shared ESP32 state
    let esp32_state = SharedEsp32State::new();

    // Create HTTP proxy with ESP32 state for intercepting WiFi/GPS requests
    let proxy = HttpProxy::new(&args.backend, &args.name, esp32_state.clone())?;

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

    // Wrap proxy in Arc for sharing across async tasks
    let proxy = std::sync::Arc::new(proxy);

    // Get ESP32 state from proxy
    let esp32_state = proxy.esp32_state();

    // Create HTTP client for forwarding GPS to Java backend
    let gps_forward_client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(5))
        .build()?;
    let backend_url = args.backend.clone();

    // Create channels for UART communication
    let (event_tx, event_rx) = mpsc::channel::<UartEvent>();
    let (command_tx, command_rx) = tokio::sync::mpsc::channel::<UartCommand>(100);

    // Store command_tx in proxy for sending commands to ESP32
    proxy.set_uart_tx(command_tx.clone()).await;

    // Spawn UART bridge
    let _uart_thread = spawn_uart_bridge(args.port.clone(), args.baud, event_tx, command_rx);

    log::info!("UART bridge started, waiting for messages from ESP32...");

    // Spawn UART heartbeat task to keep ESP32 watchdog happy
    // Only sends heartbeat if Java backend is reachable (confirms whole stack is healthy)
    let heartbeat_tx = command_tx.clone();
    let heartbeat_proxy = proxy.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(tokio::time::Duration::from_secs(5));
        loop {
            interval.tick().await;

            // Check if Java backend is responding before sending heartbeat
            // This ensures we only pet the watchdog when the whole stack is healthy
            if heartbeat_proxy.check_health().await {
                let heartbeat_cmd = r#"{"cmd":"wdt_pet"}"#.to_string();
                if let Err(e) = heartbeat_tx.send(UartCommand::Send(heartbeat_cmd)).await {
                    log::error!("Failed to send UART heartbeat: {}", e);
                    break;
                }
                log::trace!("UART heartbeat sent to ESP32 (backend healthy)");
            } else {
                log::warn!("Skipping UART heartbeat - Java backend not responding");
            }
        }
    });

    // Main event loop
    loop {
        match event_rx.try_recv() {
            Ok(event) => {
                match event {
                    UartEvent::Started { port } => {
                        log::info!("UART ready: {}", port);
                    }
                    UartEvent::MessageReceived(message) => {
                        // Log all non-HTTP messages at info level for debugging
                        if !message.contains("\"type\":\"http\"") {
                            log::info!("UART non-HTTP message: {}", message);
                        }

                        // First, try to parse as ESP32 state message (GPS, WiFi, heartbeat, etc.)
                        if let Ok(esp32_msg) = serde_json::from_str::<Esp32Message>(&message) {
                            log::info!("Parsed as Esp32Message: {:?}", esp32_msg);
                            // Forward GPS to Java backend for fallback speed
                            if let Esp32Message::Gps(ref gps) = esp32_msg {
                                log::info!(
                                    "Forwarding GPS to Java: fix={}, sats={}, lat={:?}, lng={:?}, speed={:?}",
                                    gps.fix, gps.sats, gps.lat, gps.lng, gps.speed
                                );
                                let client = gps_forward_client.clone();
                                let url = format!("{}/planter_app/rest/updateGpsFromBridge", backend_url);
                                let gps_json = serde_json::to_string(gps).unwrap_or_default();
                                tokio::spawn(async move {
                                    match client
                                        .post(&url)
                                        .header("Content-Type", "application/json")
                                        .body(gps_json)
                                        .send()
                                        .await
                                    {
                                        Ok(resp) => log::debug!("GPS forward response: {}", resp.status()),
                                        Err(e) => log::warn!("Failed to forward GPS to Java: {}", e),
                                    }
                                });
                            }
                            handle_esp32_message(&esp32_state, esp32_msg);
                            continue;
                        }

                        // Otherwise, parse as app request and proxy to backend
                        match serde_json::from_str::<AppRequest>(&message) {
                            Ok(request) => {
                                // Spawn HTTP request handling as separate task to not block main loop
                                // This allows GPS messages to be processed while HTTP requests are pending
                                let proxy_clone = proxy.clone();
                                let command_tx_clone = command_tx.clone();
                                tokio::spawn(async move {
                                    // Handle request (may be intercepted for WiFi/GPS)
                                    let responses = proxy_clone.handle_request(request).await;

                                    // Send responses back via UART
                                    for response in responses {
                                        match serde_json::to_string(&response) {
                                            Ok(json) => {
                                                if let Err(e) =
                                                    command_tx_clone.send(UartCommand::Send(json)).await
                                                {
                                                    log::error!("Failed to send response: {}", e);
                                                }
                                            }
                                            Err(e) => {
                                                log::error!("Failed to serialize response: {}", e);
                                            }
                                        }
                                    }
                                });
                            }
                            Err(e) => {
                                // Not a recognized message format - could be debug output
                                log::trace!("Unrecognized message: {} ({})", message, e);
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

/// Handle messages from ESP32 (GPS, WiFi status, etc.)
fn handle_esp32_message(state: &SharedEsp32State, msg: Esp32Message) {
    match msg {
        Esp32Message::Gps(gps) => {
            log::debug!(
                "GPS update: fix={}, sats={}, lat={:?}, lng={:?}",
                gps.fix,
                gps.sats,
                gps.lat,
                gps.lng
            );
            state.update_gps(gps);
        }
        Esp32Message::Wifi(wifi) => {
            log::info!(
                "WiFi update: connected={}, ssid={:?}, ip={:?}",
                wifi.connected,
                wifi.ssid,
                wifi.ip
            );
            state.update_wifi(wifi);
        }
        Esp32Message::WifiConfigAck { status, msg } => {
            log::info!("WiFi config ack: status={}, msg={:?}", status, msg);
        }
        Esp32Message::Heartbeat {
            uptime,
            gps_fix,
            sats,
            ble_connected,
            wifi_connected,
        } => {
            log::debug!(
                "Heartbeat: uptime={}s, gps_fix={}, sats={:?}, ble={}, wifi={:?}",
                uptime,
                gps_fix,
                sats,
                ble_connected,
                wifi_connected
            );
        }
        Esp32Message::Boot { msg } => {
            log::info!("ESP32 boot: {}", msg);
        }
        Esp32Message::Init { step } => {
            log::info!("ESP32 init: {}", step);
        }
        Esp32Message::Startup {
            device,
            version,
            mode,
        } => {
            log::info!("ESP32 startup: {} v{} ({})", device, version, mode);
        }
        Esp32Message::Ready { ble } => {
            log::info!("ESP32 ready: ble={}", ble);
        }
        Esp32Message::Error { msg } => {
            log::error!("ESP32 error: {}", msg);
        }
    }
}

async fn run_ble_mode(
    args: Args,
    proxy: HttpProxy,
) -> Result<(), Box<dyn std::error::Error>> {
    log::info!("Running in BLE mode (legacy)");
    log::info!("Device name: {}", args.name);
    log::warn!("BLE mode does not support ESP32 WiFi/GPS - use UART mode instead");

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
