#!/usr/bin/env bash
# Creates the key a release is signed with (#583): ECDSA P-256, the private
# half protected by a passphrase that openssl asks for on the terminal.
#
#   flashing/make-release-key.sh
#
# Writes, in ~/.config/split-flap (or $SPLITFLAP_KEY_DIR):
#   release-key.pem      private, encrypted, mode 600 — never leaves this machine
#                        except as a backup
#   release-key.pub.pem  public — what the firmware carries
#
# The private key exists unencrypted only in this process's memory. An
# existing key is never replaced: every wall trusts the one it was built with.
set -euo pipefail
umask 077

dir=${SPLITFLAP_KEY_DIR:-$HOME/.config/split-flap}
key=$dir/release-key.pem
pub=$dir/release-key.pub.pem

if [ -e "$key" ] || [ -e "$pub" ]; then
  echo "A release key already exists in $dir — not replacing it." >&2
  exit 1
fi
if [ ! -t 0 ]; then
  echo "Run this from a terminal: openssl has to ask for the passphrase." >&2
  exit 1
fi

mkdir -p "$dir"
chmod 700 "$dir"

plain=$(openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256)

echo "Choose a passphrase for the release key (asked twice)."
if ! openssl pkey -aes256 -out "$key" <<<"$plain"; then
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
echo "Keep a copy of the private key and its passphrase off this machine."
