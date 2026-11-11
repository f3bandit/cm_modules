#!/bin/sh
# Builds Windows and Linux versions (macOS needs cgo for USB port details: build it on a Mac) into dist/. Needs Go 1.26+ (go.mod pins toolchain go1.27.3).
# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md).
set -e
cd "$(dirname "$0")"
mkdir -p dist
export CGO_ENABLED=0
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/cm-link.exe .
GOOS=linux   GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/cm-link-linux .
# the Claude plugin (the marketplace in the repo root lists it)
cp dist/cm-link.exe plugin/server/cm-link.exe
# the notices of the Go libraries in it travel with it
cp THIRD_PARTY_NOTICES.md plugin/server/THIRD_PARTY_NOTICES.md
ls -l dist plugin/server
