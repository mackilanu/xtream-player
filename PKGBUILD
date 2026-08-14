# Maintainer: Marcus Andersson <18679427+mackilanu@users.noreply.github.com>

pkgname=xtream-player
pkgver=0.2.0
pkgrel=1
pkgdesc='Native GTK 4 player for Xtream Codes IPTV providers'
arch=('x86_64')
url='https://github.com/mackilanu/xtream-player'
license=('MIT')
depends=(
  'gst-plugin-gtk4'
  'gst-plugins-bad'
  'gst-plugins-good'
  'gstreamer'
  'gtk4'
  'json-glib'
  'libadwaita'
  'libsecret'
  'libsoup3'
)
makedepends=('meson' 'ninja')
_commit='853c15e633a7607d78ad4cc895805986c0af1c6f'
source=("$pkgname-$pkgver.tar.gz::$url/archive/$_commit.tar.gz")
sha256sums=('995fbf548144c8e1f0d6f41be44901dfe31055822d29d7b163741d393506ca7d')

build() {
  arch-meson "$pkgname-$_commit" build
  meson compile -C build
}

package() {
  meson install -C build --destdir "$pkgdir"
  install -Dm644 "$pkgname-$_commit/LICENSE" \
    "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
