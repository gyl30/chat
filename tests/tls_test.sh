#!/usr/bin/env bash
set -euo pipefail

# Ephemeral credentials for the existing server/SDK fixture; never an insecure client mode.
tls_fixture=$(mktemp -d "${TMPDIR:-/tmp}/chat-tls-test.XXXXXXXX")
trap 'tls_result=$?; if (( tls_result != 0 )); then tail -n 20 "$tls_fixture/openssl.log" 2>/dev/null || true; fi; rm -rf -- "$tls_fixture"' EXIT
mkdir "$tls_fixture/empty-store"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
    -subj /CN=Chat-Test-CA -addext basicConstraints=critical,CA:TRUE \
    -addext keyUsage=critical,keyCertSign,cRLSign \
    -keyout "$tls_fixture/ca.key" -out "$tls_fixture/ca.pem" > "$tls_fixture/openssl.log" 2>&1
for tls_leaf in good wrong expired; do
    openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -subj /CN=chat-test -keyout "$tls_fixture/$tls_leaf.key" \
        -out "$tls_fixture/$tls_leaf.csr" >> "$tls_fixture/openssl.log" 2>&1
    tls_days=2
    tls_san='DNS:localhost,IP:127.0.0.1,IP:::1'
    if [[ $tls_leaf == wrong ]]; then tls_san='DNS:wrong.invalid'; fi
    if [[ $tls_leaf == expired ]]; then tls_days=-1; fi
    cat > "$tls_fixture/extensions" <<EXT
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=$tls_san
EXT
    openssl x509 -req -in "$tls_fixture/$tls_leaf.csr" -CA "$tls_fixture/ca.pem" \
        -CAkey "$tls_fixture/ca.key" -CAcreateserial -days "$tls_days" \
        -extfile "$tls_fixture/extensions" -out "$tls_fixture/$tls_leaf.pem" \
        >> "$tls_fixture/openssl.log" 2>&1
    cat "$tls_fixture/ca.pem" >> "$tls_fixture/$tls_leaf.pem"
done
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
    -subj /CN=localhost -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    -keyout "$tls_fixture/untrusted.key" -out "$tls_fixture/untrusted.pem" \
    >> "$tls_fixture/openssl.log" 2>&1
printf 'invalid PEM\n' > "$tls_fixture/malformed.pem"
cat "$tls_fixture/good.pem" > "$tls_fixture/trailing.pem"
printf 'invalid trailing data\n' >> "$tls_fixture/trailing.pem"
SSL_CERT_FILE="$tls_fixture/ca.pem" SSL_CERT_DIR="$tls_fixture/empty-store" \
    "$1" --tls-only "$tls_fixture" </dev/null
