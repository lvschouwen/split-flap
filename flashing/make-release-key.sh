#!/usr/bin/env bash
# Creates the key a release is signed with (#583): ECDSA P-256, the private
# half protected by a passphrase that openssl asks for on the terminal.
#
#   flashing/make-release-key.sh [--no-passphrase | --passphrase-file FILE]
#
# --passphrase-file takes the passphrase from the first line of FILE instead
# of asking; whoever can read that file and the key can sign.
# --no-passphrase writes the private half unencrypted: a release can then be
# signed with nobody at the keyboard, and the file alone is enough to sign.
#
# Writes, in ~/.config/split-flap (or $SPLITFLAP_KEY_DIR):
#   release-key.pem      private, mode 600 — never leaves this machine
#                        except as a backup
#   release-key.pub.pem  public — what the firmware carries
#
# With a passphrase the private key exists unencrypted only in this process's
# memory. An existing key is never replaced: every wall trusts the one it was built with.
set -euo pipefail
umask 077

dir=${SPLITFLAP_KEY_DIR:-$HOME/.config/split-flap}
key=$dir/release-key.pem
pub=$dir/release-key.pub.pem

if [ -e "$key" ] || [ -e "$pub" ]; then
  echo "A release key already exists in $dir — not replacing it." >&2
  exit 1
fi
# openssl's default is 2048 rounds, which makes a copied key file cheap to
# guess passphrases against.
KDF_ROUNDS=600000
plainKey=0
passFile=
case "${1:-}" in
  "") ;;
  --no-passphrase) plainKey=1 ;;
  --passphrase-file)
    passFile=${2:-}
    [ -r "$passFile" ] || { echo "cannot read the passphrase file" >&2; exit 2; } ;;
  *) echo "usage: $0 [--no-passphrase | --passphrase-file FILE]" >&2; exit 2 ;;
esac
if [ "$plainKey" = 0 ] && [ -z "$passFile" ] && [ ! -t 0 ]; then
  echo "Run this from a terminal: openssl has to ask for the passphrase." >&2
  exit 1
fi

mkdir -p "$dir"
chmod 700 "$dir"

plain=$(openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256)

if [ "$plainKey" = 1 ]; then
  openssl pkey -out "$key" <<<"$plain"
elif [ -n "$passFile" ]; then
  openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf hmacWithSHA256 \
    -iter "$KDF_ROUNDS" -passout "file:$passFile" -out "$key" <<<"$plain"
elif ! { echo "Choose a passphrase for the release key (asked twice)."; \
         openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf hmacWithSHA256 \
           -iter "$KDF_ROUNDS" -out "$key" <<<"$plain"; }; then
  rm -f "$key"
  echo "No key written." >&2
  exit 1
fi
openssl pkey -pubout -out "$pub" <<<"$plain"
unset plain
chmod 600 "$key"
chmod 644 "$pub"

echo
echo "Private key : $key"
echo "Public key  : $pub"
echo "Fingerprint : $(openssl pkey -pubin -in "$pub" -outform DER | sha256sum | cut -d' ' -f1)"
echo
echo "Keep a copy of the private key (and its passphrase, if it has one) off this machine."
