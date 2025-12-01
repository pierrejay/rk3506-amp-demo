# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

# DMX Gateway Makefile

BINARY_NAME=dmx-gw
VERSION?=1.0.0
BUILD_TIME=$(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

# Luckfox target
LUCKFOX_IP?=192.168.0.132
LUCKFOX_USER?=root
LUCKFOX_PASS?=luckfox

# Go parameters
GOCMD=go
GOBUILD=$(GOCMD) build
GOCLEAN=$(GOCMD) clean
GOTEST=$(GOCMD) test
GOGET=$(GOCMD) get
GOMOD=$(GOCMD) mod

# Build flags
LDFLAGS=-ldflags "-s -w -X main.Version=$(VERSION) -X main.BuildTime=$(BUILD_TIME)"

# Targets
.PHONY: all build build-arm build-linux clean test deps run help ssh-setup deploy

all: build

## Build for current platform
build:
	$(GOBUILD) $(LDFLAGS) -o $(BINARY_NAME) .

## Build for ARM (Luckfox Lyra)
build-arm:
	GOOS=linux GOARCH=arm GOARM=7 $(GOBUILD) $(LDFLAGS) -o $(BINARY_NAME)-arm .

## Build for Linux x86_64
build-linux:
	GOOS=linux GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(BINARY_NAME)-linux .

## Build all platforms
build-all: build build-arm build-linux

## Clean build artifacts
clean:
	$(GOCLEAN)
	rm -f $(BINARY_NAME)
	rm -f $(BINARY_NAME)-arm
	rm -f $(BINARY_NAME)-linux

## Run tests
test:
	$(GOTEST) -v ./...

## Download dependencies
deps:
	$(GOMOD) download
	$(GOMOD) tidy

## Run locally (dev mode)
run: build
	./$(BINARY_NAME) -config config.example.yaml -log-level DEBUG

## Setup SSH key on Luckfox (run once after reflash)
ssh-setup:
	@echo "Setting up SSH key on $(LUCKFOX_IP)..."
	sshpass -p '$(LUCKFOX_PASS)' ssh-copy-id -i ~/.ssh/id_ed25519.pub $(LUCKFOX_USER)@$(LUCKFOX_IP)
	@echo "SSH key installed! You can now use 'make deploy' without password."

## Deploy to Luckfox (builds, copies binary + config, restarts service)
deploy: build-arm
	@echo "Deploying to $(LUCKFOX_IP)..."
	ssh $(LUCKFOX_USER)@$(LUCKFOX_IP) 'mkdir -p /etc/dmx-gw'
	ssh $(LUCKFOX_USER)@$(LUCKFOX_IP) 'killall dmx-gw 2>/dev/null || true'
	scp $(BINARY_NAME)-arm $(LUCKFOX_USER)@$(LUCKFOX_IP):/usr/bin/$(BINARY_NAME)
	scp config.yaml $(LUCKFOX_USER)@$(LUCKFOX_IP):/etc/dmx-gw/config.yaml
	@echo "Deployed to $(LUCKFOX_IP)"
	@echo "Start with: ssh $(LUCKFOX_USER)@$(LUCKFOX_IP) '/usr/bin/dmx-gw -config /etc/dmx-gw/config.yaml &'"

## Deploy and start
deploy-run: deploy
	ssh $(LUCKFOX_USER)@$(LUCKFOX_IP) 'nohup /usr/bin/dmx-gw -config /etc/dmx-gw/config.yaml > /var/log/dmx-gw.log 2>&1 &'
	@sleep 1
	@echo "Started! Testing..."
	curl -s http://$(LUCKFOX_IP):8080/api/health | head -c 100
	@echo ""

## Show help
help:
	@echo "DMX Gateway - Build Targets"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@grep -E '^## ' Makefile | sed 's/## /  /'
	@echo ""
	@echo "Variables:"
	@echo "  LUCKFOX_IP   - Target IP (default: $(LUCKFOX_IP))"
	@echo "  LUCKFOX_USER - SSH user (default: $(LUCKFOX_USER))"
	@echo "  LUCKFOX_PASS - SSH password for ssh-setup (default: $(LUCKFOX_PASS))"
	@echo "  VERSION      - Version string (default: $(VERSION))"
	@echo ""
	@echo "After reflash workflow:"
	@echo "  1. make ssh-setup    # Install SSH key (once)"
	@echo "  2. make deploy-run   # Build, deploy, and start"
