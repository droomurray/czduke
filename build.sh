#!/bin/sh
# Builds dist/czduke_<version>_arm64.deb with Docker. Works against a remote
# Docker host too:  DOCKER_HOST=ssh://user@host ./build.sh
set -eu
cd "$(dirname "$0")"
DOCKER_BUILDKIT=1 docker build --output type=local,dest=dist .
ls -l dist/*.deb
