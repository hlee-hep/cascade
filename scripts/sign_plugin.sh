#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <plugin.so> <private_key_pem>" >&2
  exit 1
fi

plugin="$1"
privkey="$2"
sig="${plugin}.sig"

if [[ ! -f "$plugin" ]]; then
  echo "Plugin not found: $plugin" >&2
  exit 1
fi

if [[ ! -f "$privkey" ]]; then
  echo "Private key not found: $privkey" >&2
  exit 1
fi

openssl pkeyutl -sign -inkey "$privkey" -rawin -in "$plugin" -out "$sig"
echo "Signature written to $sig"
