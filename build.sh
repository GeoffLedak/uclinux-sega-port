#!/usr/bin/env bash
set -e

docker run --rm \
  -v "$PWD":/home/ubuntu/Documents/projects/uclinux-sega-port \
  -w /home/ubuntu/Documents/projects/uclinux-sega-port \
  ubuntu12-68katy \
  make "$@"
