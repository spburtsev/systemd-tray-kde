# systemd-tray

`systemd-tray` is a small Qt 6 tray application for monitoring and controlling one systemd service. Run multiple instances to monitor multiple services.

It talks directly to systemd over D-Bus using `libsystemd`/sd-bus.

## Features

- Monitor system or user services.
- Show the service state in the tooltip and tray menu.
- Overlay a status dot on the selected icon:
  - green: active;
  - dark grey: inactive or transitioning;
  - red: failed;
  - no dot: status unavailable.
- Start, stop, and restart the service through systemd D-Bus.
- Update immediately from systemd signals, with periodic refresh as a fallback.
- Use a custom PNG, SVG, or other Qt-supported image as the base icon.
- Follow the service journal in Konsole.

## Requirements

- Linux with systemd and a graphical system tray.
- A C++23 compiler.
- CMake 3.20 or newer.
- Qt 6 Widgets development files.
- `pkg-config` and the `libsystemd` development files.
- Konsole and `journalctl` for the optional **Follow logs** action.

## Usage

```text
systemd-tray [options] service
```

Options:

```text
-l, --label <label>       Display label
-u, --user                Monitor a user service instead of a system service
-i, --interval <seconds>  Fallback refresh interval (default: 5)
    --icon <path>          Base tray icon file
```

Monitor a system service:

```bash
./build-release/systemd-tray postgresql.service
```

Use a custom label and icon:

```bash
./build-release/systemd-tray \
  --label PostgreSQL \
  --icon /path/to/postgresql.svg \
  postgresql.service
```

Monitor a user service:

```bash
./build-release/systemd-tray --user syncthing.service
```

To monitor several services, start one instance for each service.

If a custom icon cannot be loaded, the application prints a warning and uses the desktop's `applications-system` icon, with Qt's computer icon as the final fallback. Relative icon paths are resolved from the application's working directory.

## Permissions

Do not run the tray application with `sudo`. Reading system-service state normally works as an unprivileged user and preserves the desktop theme and session environment.

Starting, stopping, or restarting a system service may trigger a Polkit authentication prompt, depending on the system policy. User-service actions normally require no elevated privileges.

## Service updates

The application keeps one persistent connection to either the system bus or user bus. It subscribes to systemd property-change signals and updates the tray without polling subprocesses. The interval option controls a lightweight D-Bus refresh used as a consistency and reconnection fallback.
