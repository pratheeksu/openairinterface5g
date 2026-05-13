#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
set -e

SUCCESS=true

echo "Building Docker target: build-tracy..."
docker build . -f ci-scripts/docker/Dockerfile.build.optional.ubuntu --target build-tracy || SUCCESS=false

echo "Building Docker target: build-imscope..."
docker build . -f ci-scripts/docker/Dockerfile.build.optional.ubuntu --target build-imscope || SUCCESS=false

if $SUCCESS; then
  echo "Both Docker builds succeeded."
  exit 0
else
  echo "One or more Docker builds failed."
  exit 1
fi
