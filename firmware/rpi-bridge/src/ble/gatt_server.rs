//! BLE GATT Server for Planter App
//!
//! Provides a BLE GATT server that the Flutter app connects to.
//! Uses a custom service with characteristics for bidirectional JSON messaging.
//!
//! Service Structure:
//! - Planter Service (custom UUID)
//!   - Request Characteristic (Write): App writes JSON requests here
//!   - Response Characteristic (Notify): Bridge sends JSON responses here

use bluer::{
    adv::Advertisement,
    gatt::{
        local::{
            characteristic_control, Application, Characteristic, CharacteristicControlEvent,
            CharacteristicNotify, CharacteristicNotifyMethod, CharacteristicWrite,
            CharacteristicWriteMethod, Service,
        },
        CharacteristicReader, CharacteristicWriter,
    },
    Uuid,
};
use futures::{future, pin_mut, StreamExt};
use std::sync::mpsc;
use std::thread;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// Custom Service UUID for Planter App
pub const PLANTER_SERVICE_UUID: Uuid = Uuid::from_u128(0xa1b2c3d4_e5f6_7890_abcd_ef1234567890);

/// Request Characteristic UUID - app writes requests here
pub const REQUEST_CHAR_UUID: Uuid = Uuid::from_u128(0xa1b2c3d4_e5f6_7890_abcd_ef1234567891);

/// Response Characteristic UUID - bridge notifies responses here
pub const RESPONSE_CHAR_UUID: Uuid = Uuid::from_u128(0xa1b2c3d4_e5f6_7890_abcd_ef1234567892);

/// Events sent from GATT server thread to main thread
#[derive(Debug, Clone)]
pub enum GattEvent {
    /// Client connected (started notifications)
    Connected,
    /// Client disconnected
    Disconnected,
    /// JSON message received from app
    MessageReceived(String),
    /// Error occurred
    Error(String),
    /// Server started successfully
    Started { adapter_address: String },
}

/// Commands sent from main thread to GATT server thread
#[derive(Debug)]
pub enum GattCommand {
    /// Send JSON message to connected app
    Send(String),
    /// Shutdown the GATT server
    Shutdown,
}

/// Spawn the GATT server in a dedicated thread with its own tokio runtime.
pub fn spawn_gatt_server(
    device_name: String,
    event_tx: mpsc::Sender<GattEvent>,
    command_rx: tokio::sync::mpsc::Receiver<GattCommand>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let rt = match tokio::runtime::Runtime::new() {
            Ok(rt) => rt,
            Err(e) => {
                let _ = event_tx.send(GattEvent::Error(format!(
                    "Failed to create tokio runtime: {}",
                    e
                )));
                return;
            }
        };

        rt.block_on(async move {
            if let Err(e) = run_gatt_server(device_name, event_tx.clone(), command_rx).await {
                let _ = event_tx.send(GattEvent::Error(format!("GATT server error: {}", e)));
            }
        });
    })
}

/// Run the GATT server (async)
async fn run_gatt_server(
    device_name: String,
    event_tx: mpsc::Sender<GattEvent>,
    mut command_rx: tokio::sync::mpsc::Receiver<GattCommand>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    log::info!("Starting BLE GATT server...");

    // Connect to BlueZ
    let session = bluer::Session::new().await?;
    let adapter = session.default_adapter().await?;
    let adapter_addr = adapter.address().await?;

    log::info!("Using adapter: {} ({})", adapter.name(), adapter_addr);

    // Power on and make discoverable (pairable=false to avoid bonding issues)
    adapter.set_powered(true).await?;
    adapter.set_discoverable(true).await?;
    adapter.set_pairable(false).await?;
    adapter.set_alias(device_name.clone()).await?;

    log::info!("Bluetooth adapter configured as '{}'", device_name);

    // Create characteristic controls for IO model
    let (req_char_control, req_char_handle) = characteristic_control();
    let (resp_char_control, resp_char_handle) = characteristic_control();

    // Build the GATT application with explicit security settings (no auth required)
    let app = Application {
        services: vec![Service {
            uuid: PLANTER_SERVICE_UUID,
            primary: true,
            characteristics: vec![
                // Request characteristic - app writes JSON requests here
                Characteristic {
                    uuid: REQUEST_CHAR_UUID,
                    write: Some(CharacteristicWrite {
                        write: true,
                        write_without_response: true,
                        method: CharacteristicWriteMethod::Io,
                        ..Default::default()
                    }),
                    control_handle: req_char_handle,
                    ..Default::default()
                },
                // Response characteristic - bridge notifies responses here
                Characteristic {
                    uuid: RESPONSE_CHAR_UUID,
                    notify: Some(CharacteristicNotify {
                        notify: true,
                        method: CharacteristicNotifyMethod::Io,
                        ..Default::default()
                    }),
                    control_handle: resp_char_handle,
                    ..Default::default()
                },
            ],
            ..Default::default()
        }],
        ..Default::default()
    };

    // Register the application
    let _app_handle = adapter.serve_gatt_application(app).await?;
    log::info!("GATT application registered");

    // Start advertising as connectable peripheral
    let adv = Advertisement {
        advertisement_type: bluer::adv::Type::Peripheral,
        service_uuids: vec![PLANTER_SERVICE_UUID].into_iter().collect(),
        local_name: Some(device_name.clone()),
        discoverable: Some(true),
        ..Default::default()
    };

    let _adv_handle = adapter.advertise(adv).await?;
    log::info!("Advertising started");

    // Notify main thread that server started
    let _ = event_tx.send(GattEvent::Started {
        adapter_address: adapter_addr.to_string(),
    });

    // State for connections
    let mut reader_opt: Option<CharacteristicReader> = None;
    let mut writer_opt: Option<CharacteristicWriter> = None;
    let mut read_buf = vec![0u8; 512]; // Buffer for incoming data
    let mut message_buffer = String::new(); // Buffer for reassembling chunked messages

    pin_mut!(req_char_control);
    pin_mut!(resp_char_control);

    // Main event loop
    loop {
        tokio::select! {
            // Handle commands from main thread
            cmd = command_rx.recv() => {
                match cmd {
                    Some(GattCommand::Send(data)) => {
                        if let Some(writer) = writer_opt.as_mut() {
                            // Send the data as bytes with newline delimiter
                            let msg = if data.ends_with('\n') {
                                data.into_bytes()
                            } else {
                                format!("{}\n", data).into_bytes()
                            };

                            if let Err(e) = writer.write_all(&msg).await {
                                log::error!("Failed to send notification: {}", e);
                                writer_opt = None;
                                let _ = event_tx.send(GattEvent::Disconnected);
                            } else {
                                // Small delay to allow BLE stack to transmit before next message.
                                // This prevents interleaving when large messages are fragmented
                                // across multiple BLE notifications.
                                tokio::time::sleep(tokio::time::Duration::from_millis(10)).await;
                            }
                        } else {
                            log::warn!("No client connected, dropping message");
                        }
                    }
                    Some(GattCommand::Shutdown) => {
                        log::info!("Received shutdown command");
                        break;
                    }
                    None => {
                        log::info!("Command channel closed");
                        break;
                    }
                }
            }

            // Handle request characteristic events (writes from app)
            evt = req_char_control.next() => {
                match evt {
                    Some(CharacteristicControlEvent::Write(req)) => {
                        log::info!(
                            "Client connected for writes: MTU={}, addr={}",
                            req.mtu(),
                            req.device_address()
                        );
                        read_buf = vec![0u8; req.mtu()];
                        reader_opt = Some(req.accept()?);
                    }
                    Some(CharacteristicControlEvent::Notify(_)) => {
                        // This shouldn't happen for write-only characteristic
                        log::warn!("Unexpected notify event on request characteristic");
                    }
                    None => {
                        log::warn!("Request characteristic control ended");
                        break;
                    }
                }
            }

            // Handle response characteristic events (notify subscriptions)
            evt = resp_char_control.next() => {
                match evt {
                    Some(CharacteristicControlEvent::Notify(notifier)) => {
                        log::info!(
                            "Client subscribed to notifications: MTU={}, addr={}",
                            notifier.mtu(),
                            notifier.device_address()
                        );
                        writer_opt = Some(notifier);
                        let _ = event_tx.send(GattEvent::Connected);
                    }
                    Some(CharacteristicControlEvent::Write(_)) => {
                        // This shouldn't happen for notify-only characteristic
                        log::warn!("Unexpected write event on response characteristic");
                    }
                    None => {
                        log::warn!("Response characteristic control ended");
                        break;
                    }
                }
            }

            // Read data from connected client
            read_res = async {
                match &mut reader_opt {
                    Some(reader) => reader.read(&mut read_buf).await,
                    None => future::pending().await,
                }
            } => {
                match read_res {
                    Ok(0) => {
                        log::info!("Client write stream ended");
                        reader_opt = None;
                    }
                    Ok(n) => {
                        // Process received data
                        if let Ok(text) = std::str::from_utf8(&read_buf[0..n]) {
                            message_buffer.push_str(text);

                            // Process complete messages (newline-delimited)
                            while let Some(pos) = message_buffer.find('\n') {
                                let message = message_buffer[..pos].trim().to_string();
                                message_buffer = message_buffer[pos + 1..].to_string();

                                if !message.is_empty() {
                                    log::debug!("Received message: {}", message);
                                    let _ = event_tx.send(GattEvent::MessageReceived(message));
                                }
                            }
                        }
                    }
                    Err(e) => {
                        log::error!("Read error: {}", e);
                        reader_opt = None;
                    }
                }
            }
        }
    }

    log::info!("GATT server shutdown complete");
    Ok(())
}
