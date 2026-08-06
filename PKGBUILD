# Maintainer: anthonyhab <bb@hab.rip>

pkgname=bb-pywalfox-host
pkgver=0.2.0
pkgrel=1
pkgdesc="Native messaging host for syncing pywal colors with Pywalfox (C version)"
arch=('x86_64')
url="https://github.com/anthonyhab/bb-pywalfox-host"
license=('MIT')
depends=('glibc' 'json-c')
makedepends=('gcc' 'json-c')
options=('!strip')
source=("$pkgname-$pkgver.tar.gz::https://github.com/anthonyhab/bb-pywalfox-host/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('dfb1de04415b3b827b3825512a12649d2be5a0009101ff9fffb989e85018fa75')

build() {
    cd "$pkgname-$pkgver"
    make
}

package() {
    cd "$pkgname-$pkgver"
    install -Dm755 bb-pywalfox-host "$pkgdir/usr/bin/bb-pywalfox-host"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}

post_install() {
    echo "==> To complete installation, run:"
    echo "==>   bb-pywalfox-host install"
    echo "==> Then restart Firefox"
}
