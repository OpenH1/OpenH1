#!/bin/sh
# Install opence-master's release (the latest, or the tag given: v0.1.8) as
# /usr/local/bin/opence-master, checked against its release key, and restart
# the master. By hand only: the master says on its admin pages when a release
# is out (MASTER_UPDATES=notify), and we look at it before installing it.
# Needs curl, openssl and python3.
set -eu

REPO=OpenCommunityEdition/opence-master-server
ASSET=opence-master-linux-amd64
# opence-master's release key (its internal/update/key.go)
RELEASE_KEY=a61b8ee032e662ff0ba1fc60f60b689343c8893bfa4db6e82f5b791030c6c486

tag=${1:-$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
	python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])')}
case "$tag" in v[0-9]*) ;; *) echo "install-master: no release tag ($tag)" >&2; exit 1 ;; esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
base="https://github.com/$REPO/releases/download/$tag"
curl -fsSL -o "$work/SHA256SUMS" "$base/SHA256SUMS"
curl -fsSL -o "$work/SHA256SUMS.sig" "$base/SHA256SUMS.sig"
curl -fsSL -o "$work/$ASSET" "$base/$ASSET"

hex() { python3 -c 'import sys; sys.stdout.buffer.write(bytes.fromhex(sys.stdin.read().strip()))'; }
printf '302a300506032b6570032100%s' "$RELEASE_KEY" | hex > "$work/key.der"
openssl pkey -pubin -inform DER -in "$work/key.der" -out "$work/key.pem"
# (the signature: hex, as OpenH1's, or base64)
if grep -qE '^[0-9a-fA-F]{128}$' "$work/SHA256SUMS.sig"; then
	hex < "$work/SHA256SUMS.sig" > "$work/sig.bin"
else
	base64 -d < "$work/SHA256SUMS.sig" > "$work/sig.bin"
fi
openssl pkeyutl -verify -pubin -inkey "$work/key.pem" -rawin -in "$work/SHA256SUMS" -sigfile "$work/sig.bin" > /dev/null
(cd "$work" && grep " \*\?$ASSET\$" SHA256SUMS | sha256sum -c --quiet -)
chmod 755 "$work/$ASSET"
"$work/$ASSET" version 2> /dev/null || "$work/$ASSET" --version 2> /dev/null || true

install -m 755 "$work/$ASSET" /usr/local/bin/opence-master.new
mv /usr/local/bin/opence-master.new /usr/local/bin/opence-master
# (a release the master installed itself, in its bin/, would be run instead
# of an older /usr/local/bin one: it is removed, so this one runs)
rm -rf /var/lib/private/opence-master/bin
if systemctl is-enabled --quiet opence-master 2> /dev/null; then
	systemctl restart opence-master
fi
echo "install-master: $tag installed"
