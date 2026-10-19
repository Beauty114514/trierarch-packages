//! KWin provider registration shared by all guest runtime backends.

pub(crate) const PROVIDER_DESKTOP_ENTRY: &str =
    include_str!("../../wayland-ime-bridge/data/trierarch-wayland-ime.desktop");
pub(crate) const GUEST_PROVIDER_DESKTOP_PATH: &str =
    "/usr/share/applications/trierarch-wayland-ime.desktop";
pub(crate) const GUEST_PROVIDER_BRIDGE_PATH: &str =
    "/opt/trierarch/wayland-ime/trierarch-wayland-ime-bridge";
pub(crate) const GUEST_PROVIDER_SOCKET_PATH: &str =
    "/tmp/trierarch-wayland-host/ime/trierarch-ime.sock";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_uses_kwin_virtual_keyboard_registration_and_shared_runtime_paths() {
        assert!(PROVIDER_DESKTOP_ENTRY.contains("X-KDE-Wayland-VirtualKeyboard=true"));
        assert!(PROVIDER_DESKTOP_ENTRY.contains(GUEST_PROVIDER_BRIDGE_PATH));
        assert!(PROVIDER_DESKTOP_ENTRY.contains(GUEST_PROVIDER_SOCKET_PATH));
        assert!(!PROVIDER_DESKTOP_ENTRY.contains("kwinrc"));
    }
}
