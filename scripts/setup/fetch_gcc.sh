#!/bin/sh
# Fetches Debian's own build of one exact GCC, the compiler Falcon is built with, and unpacks it
# into a folder in the repository's root that git ignores, without installing it. Nothing of the
# system changes, so no system update can change this compiler, and it needs no root. The files
# come from snapshot.debian.org, which keeps every version of every Debian package.
# With --where it fetches nothing and prints the compiler's folder, for the scripts that need it.
set -eu

# The checksums below belong to this version: another version needs another list.
GCC_PACKAGE_VERSION=16.2.0-4
# Any time at which Debian's archive held that version.
SNAPSHOT_TIME=20261101T000000Z

repository_directory=$(cd "$(dirname "$0")/../.." && pwd)
install_directory=$repository_directory/gcc-$GCC_PACKAGE_VERSION
unpack_directory=$install_directory.unpacking
download_directory=$install_directory.download
pool_url=https://snapshot.debian.org/archive/debian/$SNAPSHOT_TIME/pool/main/g/gcc-16

if [ "${1:-}" = "--where" ]; then
    echo "$install_directory"
    exit 0
fi

if [ -x "$install_directory/usr/bin/g++-16" ]; then
    echo "already in $install_directory:"
    "$install_directory/usr/bin/g++-16" --version
    exit 0
fi

rm -rf "$install_directory" "$unpack_directory" "$download_directory"
mkdir -p "$unpack_directory" "$download_directory"
cd "$download_directory"

cat > SHA256SUMS <<'EOF'
fc7300ad48450b863d934f602f6817b4f09104e95ccf45d6a0af13c657c901b4  cpp-16-x86-64-linux-gnu_16.2.0-4_amd64.deb
72d548f99d3245559954fc0dfb47015002e6217d78be0249c74ad50653a4d73f  gcc-16-x86-64-linux-gnu_16.2.0-4_amd64.deb
9c2ad0a1367c9f97ae486ac2bdd87ff9494af4ac1f2b854bc520ddb2b12687a7  g++-16-x86-64-linux-gnu_16.2.0-4_amd64.deb
b3e91a02238520ad6c50cbd089de073d3ee2f0a06c496bc54177e03def27ca54  cpp-16_16.2.0-4_amd64.deb
c489d9d9c120e469f58d056945b3a91e49d6a5441fc556963229783e6ab6d819  gcc-16_16.2.0-4_amd64.deb
ab6651399356f18fbe009ea13f6d30057acc5bece4b182639ebd0356a0e9beb7  g++-16_16.2.0-4_amd64.deb
a63203467281ca8f62643f6ea5ceb13afd26c11941741f618886b64121829a81  libgcc-16-dev_16.2.0-4_amd64.deb
a850688c171492cadd7be8ef9741fcd45fe6c42b928efe4e34f6960e2e5dbf0b  libstdc++-16-dev_16.2.0-4_amd64.deb
cef6654bba1daece0bc0dbe0e3f69fa5a26eb6ba139f3536e6f4806bd787f3e7  libstdc++6_16.2.0-4_amd64.deb
d251ea642c3795e8723ac2b9eeebe511467ca04590ba70f3d555a7616787d7e3  libgcc-s1_16.2.0-4_amd64.deb
11cb09fd79c2de0acbdc23b0eb350445c563b16a36d879b355dfd45a5d2d6d3e  libgomp1_16.2.0-4_amd64.deb
453b948b39ec0014c27609fd1ea163dcc56f8f73322f5e347c42754dedfb54e9  libitm1_16.2.0-4_amd64.deb
dbdb5dc514ef881926ff4958d2c312d4fd29f0cc68c8ecb763e9138de338030e  libatomic1_16.2.0-4_amd64.deb
abed8389d22b86825c804d3eec680dd55e86d47729825c76a2475e64b0152c2c  libasan8_16.2.0-4_amd64.deb
f6391c1f8c2c063807de92b9d98a7e0b632bef4e7dd20ac34a6b77160e5b8683  liblsan0_16.2.0-4_amd64.deb
fea7dd754a62af902d0495d42db3e3b52aef91d455b9d770d682080511766881  libtsan2_16.2.0-4_amd64.deb
6a37169e525055fcfaed5b54f7bc1c93babb2338c9b9deff62f2613bdfc04f8d  libubsan1_16.2.0-4_amd64.deb
143c1d7b7d0e62ebc273494b0d5c1a6fe809b0276c77c14dace5b61e11519855  libhwasan0_16.2.0-4_amd64.deb
e1d1ad037a083ad337047e3f7d9bf786f262f79745109a756e50f8e3ec85ca1f  libquadmath0_16.2.0-4_amd64.deb
EOF

while read -r checksum package_file; do
    echo "fetching $package_file"
    curl -fsSL --retry 3 -o "$package_file" "$pool_url/$package_file"
done < SHA256SUMS
sha256sum -c --quiet SHA256SUMS

while read -r checksum package_file; do
    dpkg-deb -x "$package_file" "$unpack_directory"
done < SHA256SUMS

# Compiled and run from the folder it was unpacked into, before that folder gets its final name:
# Debian builds GCC to sit in /usr, so this shows that it finds its own parts from elsewhere.
cat > smoke_test.cpp <<'EOF'
#include <meta>
#include <print>
struct Probe {};
int main() { std::println("gcc {} reflects {}", __VERSION__, std::meta::identifier_of(^^Probe)); }
EOF
"$unpack_directory/usr/bin/g++-16" -std=c++26 -freflection -static-libstdc++ -static-libgcc \
    smoke_test.cpp -o smoke_test
./smoke_test

cd "$repository_directory"
mv "$unpack_directory" "$install_directory"
rm -rf "$download_directory"

echo "unpacked into $install_directory"
