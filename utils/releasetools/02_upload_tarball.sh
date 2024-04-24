#!/bin/bash
if [ $# != "1" ]
then
    echo "Usage: ./utils/releasetools/02_upload_tarball.sh <version_tag>"
    exit 1
fi

echo "Uploading..."
scp /tmp/valkey-${1}.tar.gz ubuntu@host.redis.io:/var/www/download/releases/
echo "Updating web site... "
echo "Please check the github action tests for the release."
echo "Press any key if it is a stable release, or Ctrl+C to abort"
read x
ssh ubuntu@host.redis.io "cd /var/www/download;
                          rm -rf valkey-${1}.tar.gz;
                          wget http://download.redis.io/releases/redis-${1}.tar.gz;
                          tar xvzf redis-${1}.tar.gz;
                          rm -rf valkey-stable;
                          mv valkey-${1} valkey-stable;
                          tar cvzf valkey-stable.tar.gz valkey-stable;
                          rm -rf valkey-${1}.tar.gz;
                          shasum -a 256 valkey-stable.tar.gz > valkey-stable.tar.gz.SHA256SUM;
                          "
