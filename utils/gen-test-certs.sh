#!/bin/bash

# Generate some test certificates which are used by the regression test suite:
#
#   tests/tls/ca.{crt,key}                       Self signed CA certificate.
#   tests/tls/ca-{expired,notyet}.crt            Self signed invalid CA certificates.
#   tests/tls/ca-expired/                        Directory containing expired CA certificate.
#   tests/tls/ca-notyet/                         Directory containing not-yet-valid CA certificate.
#   tests/tls/ca-multi.crt                       CA bundle with multiple certs.
#   tests/tls/ca-dir/                            CA directory with hashed links.
#   tests/tls/valkey.{crt,key}                   A certificate with no key usage/policy restrictions.
#   tests/tls/client.{crt,key}                   A certificate restricted for SSL client usage.
#   tests/tls/client-{expired,notyet}.crt        Invalid certificates restricted for SSL client usage.
#   tests/tls/server.{crt,key}                   A certificate restricted for SSL server usage.
#   tests/tls/server-{expired,notyet}.crt        Invalid certificates restricted for SSL server usage.
#   tests/tls/client-nul-cn.{crt,key}            Client certificate whose CN contains an embedded NUL.
#   tests/tls/valkey.dh                          DH Params file.

generate_cert() {
    local name=$1
    local cn="$2"
    local opts="$3"

    local keyfile=tests/tls/${name}.key
    local certfile=tests/tls/${name}.crt

    [ -f $keyfile ] || openssl genrsa -out $keyfile 2048
    openssl req \
        -new -sha256 \
        -subj "/O=Valkey Test/CN=$cn" \
        -key $keyfile | \
        openssl x509 \
            -req -sha256 \
            -CA tests/tls/ca.crt \
            -CAkey tests/tls/ca.key \
            -CAserial tests/tls/ca.txt \
            -CAcreateserial \
            -days 365 \
            $opts \
            -out $certfile
}

mkdir -p tests/tls
[ -f tests/tls/ca.key ] || openssl genrsa -out tests/tls/ca.key 4096
openssl req \
    -x509 -new -nodes -sha256 \
    -key tests/tls/ca.key \
    -days 3650 \
    -subj '/O=Valkey Test/CN=Certificate Authority' \
    -out tests/tls/ca.crt

cat > tests/tls/openssl.cnf <<_END_
[ server_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = server
subjectAltName = IP:127.0.0.1, IP:::1, DNS:localhost

[ client_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = client
subjectAltName = URI:urn:valkey:user:first, URI:urn:valkey:user:second

[ generic_cert ]
subjectAltName = IP:127.0.0.1, IP:::1, DNS:localhost
_END_

generate_cert server "Server-only" "-extfile tests/tls/openssl.cnf -extensions server_cert"
generate_cert client "Client-only" "-extfile tests/tls/openssl.cnf -extensions client_cert"
generate_cert valkey "Generic-cert" "-extfile tests/tls/openssl.cnf -extensions generic_cert"

# A client certificate with the CN "Client-only\0attacker", which anything
# reading the CN as a C string sees as "Client-only".
#
# The openssl CLI will not put a NUL in a name, so issue with a placeholder
# byte, overwrite it with a NUL, and re-sign tbsCertificate. The signature is
# the same length, so the DER layout is unchanged.
generate_cert client-nul-cn "Client-only@attacker" "-extfile tests/tls/openssl.cnf -extensions client_cert"
python3 - tests/tls/client-nul-cn.crt tests/tls/ca.key 'Client-only@' <<'_PYEND_'
import base64, re, subprocess, sys

cert_path, ca_key_path, marker = sys.argv[1:4]

pem = open(cert_path).read()
der = bytearray(base64.b64decode(re.sub(r"-----[^-]*-----|\s", "", pem)))

# Overwrite the last byte of the marker with a NUL.
der[der.index(marker.encode()) + len(marker) - 1] = 0

# tbsCertificate and signatureValue are direct children of Certificate.
fields = subprocess.run(["openssl", "asn1parse", "-in", cert_path],
                        capture_output=True, text=True, check=True).stdout.splitlines()

def span(line):
    """(element start, content start, content end)"""
    off, hl, length = map(int, re.match(r"\s*(\d+):d=1\s+hl=\s*(\d+)\s+l=\s*(\d+)", line).groups())
    return off, off + hl, off + hl + length

tbs_start, _, tbs_end = span(fields[1])
_, sig_start, sig_end = span([f for f in fields if "BIT STRING" in f][-1])

# The signature covers tbsCertificate including its header.
open(cert_path + ".tbs", "wb").write(bytes(der[tbs_start:tbs_end]))
subprocess.run(["openssl", "dgst", "-sha256", "-sign", ca_key_path,
                "-out", cert_path + ".sig", cert_path + ".tbs"], check=True)
sig = open(cert_path + ".sig", "rb").read()

# Skip the BIT STRING's unused-bit count octet.
assert sig_end - sig_start - 1 == len(sig), "signature length changed"
der[sig_start + 1:sig_end] = sig

b64 = base64.b64encode(bytes(der)).decode()
with open(cert_path, "w") as out:
    out.write("-----BEGIN CERTIFICATE-----\n")
    for i in range(0, len(b64), 64):
        out.write(b64[i:i + 64] + "\n")
    out.write("-----END CERTIFICATE-----\n")
_PYEND_
rm -f tests/tls/client-nul-cn.crt.tbs tests/tls/client-nul-cn.crt.sig

# Create a CA bundle and hashed CA directory used by TLS tests.
# (ca-multi.crt and ca-dir/)
cat tests/tls/ca.crt tests/tls/server.crt > tests/tls/ca-multi.crt

ca_dir="tests/tls/ca-dir"
rm -rf "$ca_dir"
mkdir -p "$ca_dir"
cp tests/tls/ca.crt "$ca_dir/ca.crt"
ca_hash=$(openssl x509 -hash -noout -in tests/tls/ca.crt)
ca_hash_old=$(openssl x509 -subject_hash_old -noout -in tests/tls/ca.crt)
ln -sf ca.crt "$ca_dir/${ca_hash}.0"
if [ "$ca_hash_old" != "$ca_hash" ]; then
    ln -sf ca.crt "$ca_dir/${ca_hash_old}.0"
fi

[ -f tests/tls/valkey.dh ] || openssl dhparam -out tests/tls/valkey.dh 2048

echo "Generating invalid TLS test certificates for fail-fast testing..."

CA_CONFIG="tests/tls/ca_temp.cnf"
cat > "$CA_CONFIG" <<EOF
[ ca ]
default_ca = CA_default

[ CA_default ]
dir              = tests/tls
database         = \$dir/index.txt
new_certs_dir    = \$dir
serial           = \$dir/serial
default_md       = sha256
policy           = policy_anything
default_days     = 1

[ policy_anything ]
countryName            = optional
stateOrProvinceName    = optional
localityName           = optional
organizationName       = optional
organizationalUnitName = optional
commonName             = supplied
emailAddress           = optional

[ server_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = server

[ client_cert ]
keyUsage = digitalSignature, keyEncipherment
nsCertType = client
EOF

touch tests/tls/index.txt
echo "01" > tests/tls/serial

# Generate expired server cert (valid Jan 1-2, 2020)
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Server-expired" \
  -key tests/tls/server.key \
  -out tests/tls/server-expired.csr

openssl ca -batch -config "$CA_CONFIG" \
  -in tests/tls/server-expired.csr \
  -cert tests/tls/ca.crt \
  -keyfile tests/tls/ca.key \
  -startdate 20200101000000Z \
  -enddate 20200102000000Z \
  -extensions server_cert \
  -out tests/tls/server-expired.crt

# Generate expired client cert (valid Jan 1-2, 2020)
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Client-expired" \
  -key tests/tls/client.key \
  -out tests/tls/client-expired.csr

openssl ca -batch -config "$CA_CONFIG" \
  -in tests/tls/client-expired.csr \
  -cert tests/tls/ca.crt \
  -keyfile tests/tls/ca.key \
  -startdate 20200101000000Z \
  -enddate 20200102000000Z \
  -extensions client_cert \
  -out tests/tls/client-expired.crt

# Generate not-yet-valid server cert (valid Jan 1-31, 2099)
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Server-notyet" \
  -key tests/tls/server.key \
  -out tests/tls/server-notyet.csr

openssl ca -batch -config "$CA_CONFIG" \
  -in tests/tls/server-notyet.csr \
  -cert tests/tls/ca.crt \
  -keyfile tests/tls/ca.key \
  -startdate 20990101000000Z \
  -enddate 20990201000000Z \
  -extensions server_cert \
  -out tests/tls/server-notyet.crt

# Generate not-yet-valid client cert (valid Jan 1-31, 2099)
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Client-notyet" \
  -key tests/tls/client.key \
  -out tests/tls/client-notyet.csr

openssl ca -batch -config "$CA_CONFIG" \
  -in tests/tls/client-notyet.csr \
  -cert tests/tls/ca.crt \
  -keyfile tests/tls/ca.key \
  -startdate 20990101000000Z \
  -enddate 20990201000000Z \
  -extensions client_cert \
  -out tests/tls/client-notyet.crt

# Generate expired CA certificate (valid Jan 1-2, 2020) using the CA to sign itself
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Certificate Authority Expired" \
  -key tests/tls/ca.key \
  -out tests/tls/ca-expired.csr

openssl ca -batch -config "$CA_CONFIG" \
  -selfsign \
  -in tests/tls/ca-expired.csr \
  -keyfile tests/tls/ca.key \
  -startdate 20200101000000Z \
  -enddate 20200102000000Z \
  -out tests/tls/ca-expired.crt

# Generate not-yet-valid CA certificate (valid Jan 1-31, 2099)
openssl req -new -sha256 \
  -subj "/O=Valkey Test/CN=Certificate Authority Not Yet Valid" \
  -key tests/tls/ca.key \
  -out tests/tls/ca-notyet.csr

openssl ca -batch -config "$CA_CONFIG" \
  -selfsign \
  -in tests/tls/ca-notyet.csr \
  -keyfile tests/tls/ca.key \
  -startdate 20990101000000Z \
  -enddate 20990201000000Z \
  -out tests/tls/ca-notyet.crt

# Create CA certificate directories for testing tls-ca-cert-dir with invalid certs
mkdir -p tests/tls/ca-expired
mkdir -p tests/tls/ca-notyet

cp tests/tls/ca-expired.crt tests/tls/ca-expired/
cp tests/tls/ca-notyet.crt tests/tls/ca-notyet/

echo "Created CA certificate test directories:"
echo "  tests/tls/ca-expired/ (contains expired CA cert)"
echo "  tests/tls/ca-notyet/ (contains not-yet-valid CA cert)"

# Clean up temporary files
rm -f tests/tls/*-expired.csr tests/tls/*-notyet.csr tests/tls/ca-expired.csr tests/tls/ca-notyet.csr
rm -f "$CA_CONFIG" tests/tls/index.txt tests/tls/index.txt.attr tests/tls/index.txt.attr.old tests/tls/index.txt.old tests/tls/serial tests/tls/serial.old
rm -f tests/tls/0[1-9].pem tests/tls/[0-9][0-9].pem

echo ""
echo "Verification of generated invalid certificates:"
for crt in tests/tls/ca-expired.crt tests/tls/ca-notyet.crt tests/tls/*-expired.crt tests/tls/*-notyet.crt; do
  if [ -f "$crt" ]; then
    echo ""
    echo "Certificate: $crt"
    openssl x509 -in "$crt" -noout -subject -dates
  fi
done
