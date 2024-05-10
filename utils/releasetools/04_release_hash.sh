#!/bin/bash
if [ $# != "1" ]
then
    echo "Usage: ./utils/releasetools/04_release_hash.sh <version_tag>"
    exit 1
fi

SHA=$(curl -s http://download.redis.io/releases/redis-${1}.tar.gz | shasum -a 256 | cut -f 1 -d' ')
ENTRY="hash valkey-${1}.tar.gz sha256 $SHA http://download.redis.io/releases/redis-${1}.tar.gz"
echo $ENTRY >> ../valkey-hashes/README
echo "Press any key to commit, Ctrl-C to abort)."
read yes
(cd ../valkey-hashes; git commit -a -m "${1} hash."; git push)
