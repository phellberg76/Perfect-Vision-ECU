//! UART Bridge for ESP32 Communication
//!
//! Receives JSON messages from ESP32 via UART and sends responses back.
//! The ESP32 handles BLE communication with the Flutter app.

use std::io::{BufRead, BufReader, Write};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

/// Events from UART
#[derive(Debug, Clone)]
pub enum UartEvent {
    /// UART port opened successfully
    Started { port: String },
    /// Message received from ESP32
    MessageReceived(String),
    /// Error occurred
    Error(String),
}

/// Commands to send via UART
#[derive(Debug)]
pub enum UartCommand {
    /// Send message to ESP32
    Send(String),
    /// Shutdown
    Shutdown,
}

/// Spawn UART bridge in a dedicated thread
pub fn spawn_uart_bridge(
    port_name: String,
    baud_rate: u32,
    event_tx: mpsc::Sender<UartEvent>,
    command_rx: tokio::sync::mpsc::Receiver<UartCommand>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        if let Err(e) = run_uart_bridge(&port_name, baud_rate, event_tx.clone(), command_rx) {
            let _ = event_tx.send(UartEvent::Error(format!("UART bridge error: {}", e)));
        }
    })
}

fn run_uart_bridge(
    port_name: &str,
    baud_rate: u32,
    event_tx: mpsc::Sender<UartEvent>,
    mut command_rx: tokio::sync::mpsc::Receiver<UartCommand>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    log::info!("Opening UART port {} at {} baud", port_name, baud_rate);

    // Open serial port with longer timeout for large responses
    let port = serialport::new(port_name, baud_rate)
        .timeout(Duration::from_millis(1000))
        .open()?;

    let _ = event_tx.send(UartEvent::Started {
        port: port_name.to_string(),
    });

    // Clone for read/write threads
    let mut write_port = port.try_clone()?;
    let read_port = port;

    // Spawn write thread
    let write_handle = thread::spawn(move || {
        // Create a simple runtime for receiving commands
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();

        rt.block_on(async {
            loop {
                match command_rx.recv().await {
                    Some(UartCommand::Send(data)) => {
                        let msg = if data.ends_with('\n') {
                            data
                        } else {
                            format!("{}\n", data)
                        };

                        if let Err(e) = write_port.write_all(msg.as_bytes()) {
                            log::error!("UART write error: {}", e);
                        } else if let Err(e) = write_port.flush() {
                            log::error!("UART flush error: {}", e);
                        } else {
                            log::debug!("UART sent: {} bytes", msg.len());
                        }
                    }
                    Some(UartCommand::Shutdown) => {
                        log::info!("UART write thread shutting down");
                        break;
                    }
                    None => {
                        log::info!("UART command channel closed");
                        break;
                    }
                }
            }
        });
    });

    // Read loop (in main thread)
    let reader = BufReader::new(read_port);
    for line in reader.lines() {
        match line {
            Ok(message) => {
                let trimmed = message.trim().to_string();
                if !trimmed.is_empty() {
                    // Filter out ESP32 boot/heartbeat messages
                    if trimmed.starts_with("{\"type\":\"boot\"")
                        || trimmed.starts_with("{\"type\":\"init\"")
                        || trimmed.starts_with("{\"type\":\"startup\"")
                        || trimmed.starts_with("{\"type\":\"ready\"")
                        || trimmed.starts_with("{\"type\":\"heartbeat\"")
                    {
                        log::debug!("ESP32: {}", trimmed);
                    } else {
                        log::debug!("UART received: {}", trimmed);
                        let _ = event_tx.send(UartEvent::MessageReceived(trimmed));
                    }
                }
            }
            Err(e) => {
                // Timeout errors are expected, ignore them
                if e.kind() != std::io::ErrorKind::TimedOut {
                    log::error!("UART read error: {}", e);
                }
            }
        }
    }

    let _ = write_handle.join();
    Ok(())
}
