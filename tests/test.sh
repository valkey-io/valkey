#!/bin/sh -ue
#
check_executable() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: $1 is not found or not executable."
        exit 1
    fi
}

VALKEY_SERVER=${VALKEY_SERVER:-valkey-server}
VALKEY_PORT=${VALKEY_PORT:-56379}
VALKEY_TLS_PORT=${VALKEY_TLS_PORT:-56443}
TEST_TLS=${TEST_TLS:-0}
SKIPS_AS_FAILS=${SKIPS_AS_FAILS:-0}
ENABLE_DEBUG_CMD=
TLS_TEST_ARGS=
SKIPS_ARG=${SKIPS_ARG:-}
VALKEY_DOCKER=${VALKEY_DOCKER:-}
TEST_RDMA=${TEST_RDMA:-0}
RDMA_TEST_ARGS=
TEST_CLUSTER=${TEST_CLUSTER:-0}
CLUSTER_TEST_ARGS=

check_executable "$VALKEY_SERVER"

# Enable debug command for redis-server >= 7.0.0 or any version of valkey-server.
VALKEY_MAJOR_VERSION="$("$VALKEY_SERVER" --version|awk -F'[^0-9]+' '{ print $2 }')"
if [ "$VALKEY_MAJOR_VERSION" -gt "6" ]; then
    ENABLE_DEBUG_CMD="enable-debug-command local"
fi

tmpdir=$(mktemp -d)
PID_FILE=${tmpdir}/libvalkey-test-valkey.pid
SOCK_FILE=${tmpdir}/libvalkey-test-valkey.sock
CONF_FILE=${tmpdir}/valkey.conf

if [ "$TEST_TLS" = "1" ]; then
    TLS_CA_CERT=${tmpdir}/ca.crt
    TLS_CA_KEY=${tmpdir}/ca.key
    TLS_CERT=${tmpdir}/valkey.crt
    TLS_KEY=${tmpdir}/valkey.key

    openssl genrsa -out ${tmpdir}/ca.key 4096
    openssl req \
        -x509 -new -nodes -sha256 \
        -key ${TLS_CA_KEY} \
        -days 3650 \
        -subj '/CN=Libvalkey Test CA' \
        -out ${TLS_CA_CERT}
    openssl genrsa -out ${TLS_KEY} 2048
    openssl req \
        -new -sha256 \
        -key ${TLS_KEY} \
        -subj '/CN=Libvalkey Test Cert' | \
        openssl x509 \
            -req -sha256 \
            -CA ${TLS_CA_CERT} \
            -CAkey ${TLS_CA_KEY} \
            -CAserial ${tmpdir}/ca.txt \
            -CAcreateserial \
            -days 365 \
            -out ${TLS_CERT}

    TLS_TEST_ARGS="--tls-host 127.0.0.1 --tls-port ${VALKEY_TLS_PORT} --tls-ca-cert ${TLS_CA_CERT} --tls-cert ${TLS_CERT} --tls-key ${TLS_KEY}"
fi

cleanup() {
  if [ -n "${VALKEY_DOCKER}" ] ; then
    docker kill valkey-test-server
  else
    set +e
    kill $(cat ${PID_FILE})
  fi
  rm -rf ${tmpdir}
}
trap cleanup INT TERM EXIT

# base config
cat > ${CONF_FILE} <<EOF
pidfile ${PID_FILE}
port ${VALKEY_PORT}
unixsocket ${SOCK_FILE}
unixsocketperm 777
appendonly no
save ""
EOF

# if not running in docker add these:
if [ ! -n "${VALKEY_DOCKER}" ]; then
cat >> ${CONF_FILE} <<EOF
daemonize yes
${ENABLE_DEBUG_CMD}
bind 127.0.0.1
EOF
fi

# if doing tls, add these
if [ "$TEST_TLS" = "1" ]; then
    cat >> ${CONF_FILE} <<EOF
tls-port ${VALKEY_TLS_PORT}
tls-ca-cert-file ${TLS_CA_CERT}
tls-cert-file ${TLS_CERT}
tls-key-file ${TLS_KEY}
EOF
fi

# if doing RDMA, add these
if [ "$TEST_RDMA" = "1" ]; then
    cat >> ${CONF_FILE} <<EOF
loadmodule ${VALKEY_RDMA_MODULE} bind=${VALKEY_RDMA_ADDR} port=${VALKEY_PORT}
EOF
RDMA_TEST_ARGS="--rdma-addr ${VALKEY_RDMA_ADDR}"
fi

echo ${tmpdir}
cat ${CONF_FILE}
if [ -n "${VALKEY_DOCKER}" ] ; then
    chmod a+wx ${tmpdir}
    chmod a+r ${tmpdir}/*
    docker run -d --rm --name valkey-test-server \
        -p ${VALKEY_PORT}:${VALKEY_PORT} \
        -p ${VALKEY_TLS_PORT}:${VALKEY_TLS_PORT} \
        -v ${tmpdir}:${tmpdir} \
        ${VALKEY_DOCKER} \
        ${VALKEY_SERVER} ${CONF_FILE}
else
    ${VALKEY_SERVER} ${CONF_FILE}
fi
# Wait until we detect the unix socket
echo waiting for server
while [ ! -S "${SOCK_FILE}" ]; do
    2>&1 echo "Waiting for server..."
    ps aux|grep valkey-server
    sleep 1;
done

# Treat skips as failures if directed
[ "$SKIPS_AS_FAILS" = 1 ] && SKIPS_ARG="${SKIPS_ARG} --skips-as-fails"

# if cluster is not available, skip cluster tests
if [ "$TEST_CLUSTER" = "1" ]; then
    CLUSTER_TEST_ARGS="${CLUSTER_TEST_ARGS} --enable-cluster-tests"
fi

${TEST_PREFIX:-} ./client_test -h 127.0.0.1 -p ${VALKEY_PORT} -s ${SOCK_FILE} ${TLS_TEST_ARGS} ${SKIPS_ARG} ${RDMA_TEST_ARGS} ${CLUSTER_TEST_ARGS}
