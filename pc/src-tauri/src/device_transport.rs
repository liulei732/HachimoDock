//! Shared control messages at the boundary between connected devices and components.
use crate::lan_agent::component_host::{self, Session};
use crate::usb_serial::{UsbConnectionStatus, UsbSerialManager};
use serde_json::Value;
use std::sync::Arc;

pub(crate) const APP_EVENT: &str = "app/event";
pub(crate) const APP_SNAPSHOT: &str = "app/snapshot";
pub(crate) const APP_SNAPSHOT_MAX: usize = 2048;

#[derive(Clone)]
pub(crate) enum Route {
    Usb { board: String, generation: u64 },
    Wifi(Arc<Session>),
}

impl Route {
    pub(crate) fn usb(status: &UsbConnectionStatus) -> Option<Self> {
        if !status.connected || status.board_device_id.is_empty() {
            return None;
        }
        let generation = status
            .connection_id
            .parse::<u64>()
            .ok()
            .filter(|id| *id != 0)?;
        Some(Self::Usb {
            board: status.board_device_id.clone(),
            generation,
        })
    }

    pub(crate) fn board(&self) -> &str {
        match self {
            Self::Usb { board, .. } => board,
            Self::Wifi(session) => session.board(),
        }
    }

    pub(crate) fn same(&self, other: &Self) -> bool {
        match (self, other) {
            (
                Self::Usb { board, generation },
                Self::Usb {
                    board: other_board,
                    generation: other_generation,
                },
            ) => board == other_board && generation == other_generation,
            (Self::Wifi(session), Self::Wifi(other)) => Arc::ptr_eq(session, other),
            _ => false,
        }
    }

    pub(crate) fn supports_asset_streams(&self) -> bool {
        matches!(self, Self::Usb { .. })
    }
}

pub(crate) struct DeviceTransport {
    usb: UsbSerialManager,
}

impl DeviceTransport {
    pub(crate) fn new(usb: UsbSerialManager) -> Self {
        Self { usb }
    }

    pub(crate) fn wired_route(&self) -> Option<Route> {
        Route::usb(&self.usb.status())
    }

    pub(crate) fn current(&self) -> Option<Route> {
        let status = self.usb.status();
        if status.connected {
            Route::usb(&status)
        } else {
            component_host::active().map(Route::Wifi)
        }
    }

    pub(crate) fn is_current(&self, route: &Route) -> bool {
        self.current().is_some_and(|current| route.same(&current))
    }

    pub(crate) fn accepts_app_event(&self, route: &Route, event: &Value) -> bool {
        if event["boardDeviceId"] != route.board() {
            return false;
        }
        match route {
            Route::Usb { .. } => {
                event.get("data").is_none()
                    || (self.usb.status().capabilities["widgetServiceBridge"]
                        == crate::service_bridge::surface::DEVICE_ENGINE
                        && crate::service_bridge::surface::validate_event_data(&event["data"]))
            }
            // Host applications with media, resource or external-service
            // operations need those capabilities wired before opting in.
            Route::Wifi(_) => wifi_app_supported(event),
        }
    }

    pub(crate) fn send_snapshot(
        &self,
        route: &Route,
        snapshot: &Value,
        can_wait: bool,
    ) -> Result<(), String> {
        if !self.is_current(route) {
            return Err("设备连接已变化".into());
        }
        match route {
            Route::Usb { board, generation } => {
                self.usb
                    .send_app_snapshot_to_generation(board, *generation, snapshot, can_wait)
            }
            Route::Wifi(session) => session.queue_snapshot(snapshot),
        }
    }
}

fn wifi_app_supported(event: &Value) -> bool {
    event["componentId"] == "codex-quota-board"
        && event.get("data").is_none()
        && event.get("contract").is_none()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn wireless_scope_requires_a_supported_component_and_plain_events() {
        assert!(wifi_app_supported(
            &json!({"componentId":"codex-quota-board","action":"status"})
        ));
        assert!(!wifi_app_supported(
            &json!({"componentId":"hachimi-notes","action":"host.save"})
        ));
        assert!(!wifi_app_supported(
            &json!({"componentId":"codex-quota-board","data":{"x":1}})
        ));
        assert!(!wifi_app_supported(
            &json!({"componentId":"codex-quota-board","contract":"abc"})
        ));
    }
}
