#!/bin/sh
# ./build.sh        cross-compile hwremap.so for the device (armhf) with Docker
# ./build.sh test   run the host tests with ASan/UBSan
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = test ]; then
    docker run --rm -v "$PWD":/w -w /w gcc:12 bash -c \
        'gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined test_hwremap.c -ldl -lpthread -o /tmp/t && /tmp/t'
else
    docker run --rm --platform linux/arm/v7 -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euc \
        'gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -std=gnu11 hwremap.c -ldl -lpthread -o hwremap.so && strip hwremap.so'
    ls -l hwremap.so
fi
