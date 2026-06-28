# systemd-tray

`systemd-tray` is a small Linux desktop utility that adds KDE Plasma tray icons for selected `systemd` services.

It is meant for desktop services like `ollama`, `postgresql`, local databases, background workers, and other daemons that you want to see and manage visually without constantly using `systemctl`.

## Features

* Show selected `systemd` services in the KDE system tray
* Display basic service state: running, stopped, failed, activating, unknown
* Start, stop, restart, and reload services from the tray menu
* Support both system and user services
* Open logs/status for a service
* Configure only the services you care about
* Uses normal `systemd` permissions and Polkit rules

## Example use cases

* Show whether `postgresql.service` is running
* Quickly restart local PostgreSQL
* Keep an eye on development services
* Replace ad-hoc terminal checks with persistent tray indicators

## Building

```bash
cmake -B build -S .
cmake --build build
```

Install locally:

```bash
cmake --install build
```

## Running

```bash
systemd-tray
```

For development, run directly from the build directory:

```bash
./build/systemd-tray
```

## KDE integration

`systemd-tray` uses the KDE/StatusNotifier tray system, so icons appear in the normal Plasma system tray.

## Permissions

Actions such as starting or stopping system services may require authentication depending on your system configuration.

User services can usually be controlled without administrator privileges.
