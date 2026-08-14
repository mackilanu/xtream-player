Name:           xtream-player
Version:        0.2.0
Release:        1%{?dist}
Summary:        Native GTK 4 player for Xtream Codes IPTV providers

License:        MIT
URL:            https://github.com/mackilanu/xtream-player
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  pkgconfig(gtk4) >= 4.10
BuildRequires:  pkgconfig(libadwaita-1) >= 1.4
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(gstreamer-1.0)
Requires:       gstreamer1-plugin-gtk4
Requires:       gstreamer1-plugins-good
Requires:       gstreamer1-plugins-bad-free
Requires:       libsecret

%description
Xtream Player is a native GTK 4 and libadwaita application for browsing and
watching authorized live IPTV services from Xtream Codes-compatible providers.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.mackilanu.XtreamPlayer.desktop
appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/io.github.mackilanu.XtreamPlayer.metainfo.xml

%files
%license LICENSE
%doc README.md CONTRIBUTING.md
%{_bindir}/xtream-player
%{_datadir}/applications/io.github.mackilanu.XtreamPlayer.desktop
%{_datadir}/icons/hicolor/512x512/apps/io.github.mackilanu.XtreamPlayer.png
%{_datadir}/metainfo/io.github.mackilanu.XtreamPlayer.metainfo.xml

%changelog
* Fri Aug 14 2026 Marcus Andersson <18679427+mackilanu@users.noreply.github.com> - 0.2.0-1
- Add provider movies, series, episodes, seeking, and playback resume
- Add persistent per-playlist Live TV favorites and reliable fullscreen control hiding
- Add audio-track and embedded-subtitle selectors

* Thu Aug 13 2026 Marcus Andersson <18679427+mackilanu@users.noreply.github.com> - 0.1.1-1
- Toggle fullscreen by double-clicking the video

* Thu Aug 13 2026 Marcus Andersson <18679427+mackilanu@users.noreply.github.com> - 0.1.0-1
- Initial RPM package
