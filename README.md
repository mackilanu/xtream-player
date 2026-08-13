# Xtream Player

[![Build](https://github.com/mackilanu/xtream-player/actions/workflows/build.yml/badge.svg)](https://github.com/mackilanu/xtream-player/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A native GTK 4 IPTV player with a GNOME-style interface. It connects to an
Xtream Codes-compatible provider, loads live channels, supports search, and
plays streams through GStreamer.

Only use playlists and streams you are authorized to access.

## Features

- Native GTK 4 and libadwaita interface
- Xtream Codes-compatible live TV
- Multiple named provider profiles
- Password storage in GNOME Keyring
- Provider categories and channel search
- Six-hour cache with offline fallback
- Non-blocking network requests and virtualized channel lists
- System, light, and dark appearance modes
- Fullscreen video with auto-hiding controls

## Install dependencies (Fedora)

```sh
sudo dnf install gcc meson ninja-build gtk4-devel libadwaita-devel \
  libsoup3-devel json-glib-devel gstreamer1-devel \
  gstreamer1-plugin-gtk4 gstreamer1-plugins-good gstreamer1-plugins-bad-free
```

The player requires the GStreamer `gtk4paintablesink` plugin at runtime.

## Build and run

```sh
meson setup build
meson compile -C build
./build/xtream-player
```

## Build an RPM (Fedora)

Install the RPM build tools once:

```sh
sudo dnf install rpm-build desktop-file-utils libappstream-glib
```

Then build both binary and source packages:

```sh
./scripts/build-rpm.sh
```

The finished packages are written under `build/rpmbuild/RPMS` and
`build/rpmbuild/SRPMS`. Install the binary package with:

```sh
sudo dnf upgrade "$(find build/rpmbuild/RPMS -type f -name 'xtream-player-*.rpm' \
  ! -name '*debuginfo*' ! -name '*debugsource*' -print -quit)"
```

Choose **New playlist**, give it a name, and enter the provider server URL
(including `http://` or `https://`), username, and password. Successful
connections are saved as reusable profiles. Passwords are stored in GNOME
Keyring; the profile name, server, and username are stored in the normal user
configuration directory.

Channel and category responses are cached for six hours under the user cache
directory. If the provider is temporarily unavailable, the most recent cached
response is used as a fallback. Network fetching and JSON parsing run on a
worker thread.

## Project status

Xtream Player is an early-stage community project. VOD, series, EPG, favorites,
and packaging are planned but not implemented yet. Bug reports and focused pull
requests are welcome.

## Privacy and legal use

Passwords are stored through GNOME Keyring and are never written to the profile
configuration file. Cached API responses may contain channel metadata.

This project does not provide channels or subscriptions. Only use services and
streams you are legally authorized to access.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and submission guidance.

## License

MIT. See [LICENSE](LICENSE).
