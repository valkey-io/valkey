#!/bin/bash

# Generate some test certificates which are used by the regression test suite:
#
#   tests/tls/ca.{crt,key}           Self signed CA certificate.
#   tests/tls/valkey.{crt,key}       A certificate with no key usage/policy restrictions.
#   tests/tls/client.{crt,key}       A certificate restricted for SSL client usage.
#   tests/tls/server.{crt,key}       A certificate restricted for SSL server usage.
#   tests/tls/server-expired.{crt,key} An expired server certificate for INFO tls tests.
#   tests/tls/ca-multi.crt           A CA bundle with multiple certs.
#   tests/tls/ca-dir/                CA directory with hashed links.
#   tests/tls/valkey.dh              DH Params file.

TLS_DIR="tests/tls"

emit_output() {
    local status="$1"
    local subject="$2"
    if [ "$status" -eq 0 ]; then
        echo "Signature ok"
    else
        echo "Signature failed"
    fi
    echo "subject=$subject"
}

init_ca() {
    mkdir -p "$TLS_DIR"
    [ -f "$TLS_DIR/ca.key" ] || openssl genrsa -out "$TLS_DIR/ca.key" 4096 >/dev/null 2>&1
    openssl req \
        -x509 -new -nodes -sha256 \
        -key "$TLS_DIR/ca.key" \
        -days 3650 \
        -subj '/O=Valkey Test/CN=Certificate Authority' \
        -out "$TLS_DIR/ca.crt" >/dev/null 2>&1
    emit_output $? "/O=Valkey Test/CN=Certificate Authority"
}

write_ext_config() {
    cat > "$TLS_DIR/openssl.cnf" <<_END_
[ server_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = server

[ client_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = client
_END_
}

generate_cert() {
    local name=$1
    local cn="$2"
    local opts="$3"

    local keyfile="$TLS_DIR/${name}.key"
    local certfile="$TLS_DIR/${name}.crt"

    [ -f "$keyfile" ] || openssl genrsa -out "$keyfile" 2048 >/dev/null 2>&1
    openssl req \
        -new -sha256 \
        -subj "/O=Valkey Test/CN=$cn" \
        -key "$keyfile" 2>/dev/null | \
        openssl x509 \
            -req -sha256 \
            -CA "$TLS_DIR/ca.crt" \
            -CAkey "$TLS_DIR/ca.key" \
            -CAserial "$TLS_DIR/ca.txt" \
            -CAcreateserial \
            -days 365 \
            $opts \
            -out "$certfile" >/dev/null 2>&1
    emit_output $? "/O=Valkey Test/CN=$cn"
}

generate_bundle() {
    cat "$TLS_DIR/ca.crt" "$TLS_DIR/server.crt" > "$TLS_DIR/ca-multi.crt"
    emit_output $? "$TLS_DIR/ca-multi.crt"
}

generate_ca_dir() {
    local ca_dir="$TLS_DIR/ca-dir"
    local ca_cert="$TLS_DIR/ca.crt"

    rm -rf "$ca_dir"
    mkdir -p "$ca_dir"
    cp "$ca_cert" "$ca_dir/ca.crt"
    local ca_hash
    ca_hash=$(openssl x509 -hash -noout -in "$ca_cert")
    local ca_hash_old
    ca_hash_old=$(openssl x509 -subject_hash_old -noout -in "$ca_cert")
    ln -sf ca.crt "$ca_dir/${ca_hash}.0"
    if [ "$ca_hash_old" != "$ca_hash" ]; then
        ln -sf ca.crt "$ca_dir/${ca_hash_old}.0"
    fi
    emit_output $? "$ca_dir"
}

generate_expired_cert() {
    local name=$1
    local cn="$2"

    local expired_dir="$TLS_DIR/ca-expired"
    local keyfile="$TLS_DIR/${name}.key"
    local csrfile="$TLS_DIR/${name}.csr"
    local certfile="$TLS_DIR/${name}.crt"

    rm -rf "$expired_dir"
    mkdir -p "$expired_dir/newcerts"
    : > "$expired_dir/index.txt"
    echo 1000 > "$expired_dir/serial"

    cat > "$expired_dir/openssl.cnf" <<_END_
[ ca ]
default_ca = CA_default

[ CA_default ]
dir = $expired_dir
database = \$dir/index.txt
new_certs_dir = \$dir/newcerts
serial = \$dir/serial
private_key = $TLS_DIR/ca.key
certificate = $TLS_DIR/ca.crt
default_md = sha256
policy = policy_any

[ policy_any ]
commonName = supplied
_END_

    openssl genrsa -out "$keyfile" 2048 >/dev/null 2>&1
    openssl req \
        -new -sha256 \
        -subj "/O=Valkey Test/CN=$cn" \
        -key "$keyfile" \
        -out "$csrfile" >/dev/null 2>&1
    openssl ca -batch \
        -config "$expired_dir/openssl.cnf" \
        -in "$csrfile" \
        -out "$certfile" \
        -startdate 20000101000000Z -enddate 20000102000000Z >/dev/null 2>&1
    emit_output $? "/O=Valkey Test/CN=$cn"
}

generate_dh_params() {
    [ -f "$TLS_DIR/valkey.dh" ] || openssl dhparam -out "$TLS_DIR/valkey.dh" 2048 >/dev/null 2>&1
    emit_output $? "$TLS_DIR/valkey.dh"
}

main() {
    init_ca
    write_ext_config

    generate_cert server "Server-only" "-extfile $TLS_DIR/openssl.cnf -extensions server_cert"
    generate_cert client "Client-only" "-extfile $TLS_DIR/openssl.cnf -extensions client_cert"
    generate_cert valkey "Generic-cert"

    generate_bundle
    generate_expired_cert server-expired "Expired Server"
    generate_ca_dir
    generate_dh_params
}

main "$@"
