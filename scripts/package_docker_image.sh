#!/bin/bash
# Build the Merlin AE Docker image once and export it as a compressed tarball.
#
# This script is for artifact maintainers preparing the release package. AE
# reviewers should load the resulting tarball with `docker load` instead of
# rebuilding the image from the Dockerfile.
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-merlin-ae}"
IMAGE_TAG="${IMAGE_TAG:-latest}"
OUTPUT="${OUTPUT:-merlin-ae-image.tar.gz}"

docker build --network=host -t "${IMAGE_NAME}:${IMAGE_TAG}" .
docker save "${IMAGE_NAME}:${IMAGE_TAG}" | gzip -c > "${OUTPUT}"
sha256sum "${OUTPUT}" > "${OUTPUT}.sha256"

ls -lh "${OUTPUT}" "${OUTPUT}.sha256"
