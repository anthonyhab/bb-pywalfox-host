# Maintainer: anthonyhab <bb@hab.rip>

pkgname=bb-pywalfox-host
pkgver=0.2.2
pkgrel=1
pkgdesc="Native messaging host for syncing pywal colors with Pywalfox (C version)"
arch=('x86_64')
url="https://github.com/anthonyhab/bb-pywalfox-host"
license=('MIT')
depends=('glibc' 'json-c')
makedepends=('gcc' 'json-c')
options=('!strip')
source=("$pkgname-$pkgver.tar.gz::https://github.com/anthonyhab/bb-pywalfox-host/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('e50aae362e30bd18f268797f7bb973442b795729d8a1afb5e5c4b2a422ae9586')

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
