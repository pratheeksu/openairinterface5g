#!/bin/bash
set -e
docker build . -f docker/Dockerfile.build.ubuntu.fast -t ran-build --progress=plain
docker build . -f docker/Dockerfile.gNB.ubuntu -t oai-gnb
docker build . -f docker/Dockerfile.nrUE.ubuntu -t oai-nr-ue

SHORT_COMMIT_SHA=$(git rev-parse --short=8 HEAD)
docker tag oai-nr-ue oai-ci/oai-nr-ue:develop-${SHORT_COMMIT_SHA}
docker tag oai-gnb oai-ci/oai-gnb:develop-${SHORT_COMMIT_SHA}
