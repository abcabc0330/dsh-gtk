# dsh-gtk

> ⚠️ **This is only a shell (front-end window).** It does **not** bundle the
> dsh backend. You must install and run the dsh web server yourself (typically
> `deepseek-harness-bin` from the AUR, or `npm i -g @deepseek-ai/dsh`), then
> point this window at it. Without a running `dsh web`, the window shows only a
> blank page.

A small native window for the [dsh](https://github.com/DeepSeek/dsh) web UI,
built with **GTK3 + WebKitGTK**, that stays resident in the system tray.

Modeled on magpie's `tray` mode: closing the window hides it to the
StatusNotifierItem instead of quitting, so the process keeps running in the
background.

## Features

- **Tray residency** — close the window and it hides to the system tray
  (StatusNotifierItem via `libayatana-appindicator`); the process stays alive.
- **Tray left-click** — shows and focuses the window.
- **Tray right-click menu** — 显示窗口 (Show window) / 退出 (Quit).
- **Launch token recovery** — if the dsh web server answers `401`, the token
  printed by `dsh web` is recovered from the user journal and the page is
  reloaded with it (kept per cookie jar, so it only happens once).
- **Keyboard** — `Ctrl+R` reload, `F11` fullscreen.
- **Waits for the StatusNotifierWatcher** (waybar / any SNI host) before
  registering the tray item, mirroring magpie's `ExecStartPre` logic.

## Requirements

- GTK3 (`gtk3`)
- WebKitGTK 4.1 (`webkit2gtk-4.1`)
- Ayatana AppIndicator (`libayatana-appindicator3`)
- **The dsh backend (not included)** — install `deepseek-harness-bin`
  from the AUR (or `npm i -g @deepseek-ai/dsh`), then run `dsh web` so it
  serves `http://127.0.0.1:3080/`
- An SNI host in the tray (waybar, etc.)

> 💡 **Tip:** `dsh-gtk` is useless on its own. It is a window that points at a
> `dsh web` server — install and start the backend first, then launch this.

## Build

```sh
gcc -O2 -Wall -Wextra -o dsh-gtk dsh-gtk.c \
    $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 ayatana-appindicator3-0.1)
```

## Usage

```sh
dsh-gtk [url]          # default http://127.0.0.1:3080/
```

The tray icon is looked up in `$XDG_DATA_HOME/dsh-gtk/dsh-tray.png`
(usually `~/.local/share/dsh-gtk/dsh-tray.png`) if present, otherwise a theme
icon name (`utilities-terminal`) is used.

## Tray behavior

| Action              | Result                                   |
| ------------------- | ---------------------------------------- |
| Close window (X)    | Hide to tray, process stays alive        |
| Tray left-click     | Show and focus the window                |
| Tray menu "显示窗口" | Show and focus the window                |
| Tray menu "退出"     | Quit the process and remove the tray item|

## Autostart (systemd user service, optional)

Mirroring magpie, wait for the StatusNotifierWatcher before starting:

```ini
[Unit]
Description=dsh-gtk (dsh web UI in tray)
After=graphical-session.target
BindsTo=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStartPre=/bin/sh -c 'i=0; while [ $i -lt 30 ]; do /usr/bin/gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.kde.StatusNotifierWatcher 2>/dev/null | grep -q true && exit 0; i=$((i+1)); sleep 1; done; exit 0'
ExecStart=/usr/bin/dsh-gtk
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
```

## License

MIT
