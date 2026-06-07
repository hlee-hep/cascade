#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <payload_file> <private_key_pem>" >&2
  exit 1
fi

payload="$1"
privkey="$2"
sig="${payload}.sig"

if [[ ! -f "$payload" ]]; then
  echo "Payload not found: $payload" >&2
  exit 1
fi

if [[ ! -f "$privkey" ]]; then
  echo "Private key not found: $privkey" >&2
  exit 1
fi

openssl pkeyutl -sign -inkey "$privkey" -rawin -in "$payload" -out "$sig"
echo "Signature written to $sig"
