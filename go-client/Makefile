NAME=go-client
BUILDDIR=build

BASEPATH := $(shell pwd)
CLIENTSRCFILE := $(BASEPATH)/cmd/awaken/
SSHCSRCFILE := $(BASEPATH)/cmd/sshc/

LDFLAGS=-w -s

CLIENTBUILD=CGO_ENABLED=0 go build -trimpath

# 自动检测当前平台
CURRENT_OS := $(shell go env GOOS)
CURRENT_ARCH := $(shell go env GOARCH)

# 默认目标：只构建当前平台
.PHONY: default
default: build-current

# 构建当前平台
.PHONY: build-current
build-current:
	@echo "Building for current platform: $(CURRENT_OS)/$(CURRENT_ARCH)"
	@rm -rf $(BUILDDIR)
	@mkdir -p $(BUILDDIR)/$(CURRENT_OS)-$(CURRENT_ARCH)
	@if [ "$(CURRENT_OS)" = "windows" ]; then \
		$(CLIENTBUILD) -ldflags "$(LDFLAGS) -H windowsgui" -o $(BUILDDIR)/$(CURRENT_OS)-$(CURRENT_ARCH)/JumpServerClient.exe $(CLIENTSRCFILE); \
		echo "Skipping sshc build on Windows (not supported)"; \
	else \
		$(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/$(CURRENT_OS)-$(CURRENT_ARCH)/JumpServerClient $(CLIENTSRCFILE); \
		$(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/$(CURRENT_OS)-$(CURRENT_ARCH)/client $(SSHCSRCFILE); \
	fi
	@if [ "$(CURRENT_OS)" = "darwin" ]; then \
		cp -R $(BASEPATH)/Scripts $(BUILDDIR)/$(CURRENT_OS)-$(CURRENT_ARCH)/; \
	fi
	@mkdir -p $(BASEPATH)/../src-tauri/resources/bin/
	@cp -R $(BUILDDIR)/* $(BASEPATH)/../src-tauri/resources/bin/
	@cp $(BASEPATH)/config.json $(BASEPATH)/../src-tauri/resources/bin/
	@if [ "$(CURRENT_OS)" = "windows" ]; then \
		cp $(BASEPATH)/putty.exe $(BASEPATH)/../src-tauri/resources/bin/windows/; \
		cp $(BASEPATH)/pkg/autoit/*.dll $(BASEPATH)/../src-tauri/resources/bin/windows/; \
	fi

# 构建所有平台 - 优化版本：在一个任务中构建所有目标
.PHONY: build-all
build-all:
	@echo "Building all platforms in one task..."
	@rm -rf $(BUILDDIR)
	@mkdir -p $(BUILDDIR)/{darwin-amd64,darwin-arm64,linux-amd64,linux-arm64,windows}
	
	@echo "Building Darwin AMD64..."
	GOARCH=amd64 GOOS=darwin $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/darwin-amd64/JumpServerClient $(CLIENTSRCFILE)
	GOARCH=amd64 GOOS=darwin $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/darwin-amd64/client $(SSHCSRCFILE)
	
	@echo "Building Darwin ARM64..."
	GOARCH=arm64 GOOS=darwin $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/darwin-arm64/JumpServerClient $(CLIENTSRCFILE)
	GOARCH=arm64 GOOS=darwin $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/darwin-arm64/client $(SSHCSRCFILE)
	
	@echo "Building Linux AMD64..."
	GOARCH=amd64 GOOS=linux $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/linux-amd64/JumpServerClient $(CLIENTSRCFILE)
	GOARCH=amd64 GOOS=linux $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/linux-amd64/client $(SSHCSRCFILE)
	
	@echo "Building Linux ARM64..."
	GOARCH=arm64 GOOS=linux $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/linux-arm64/JumpServerClient $(CLIENTSRCFILE)
	GOARCH=arm64 GOOS=linux $(CLIENTBUILD) -ldflags "$(LDFLAGS)" -o $(BUILDDIR)/linux-arm64/client $(SSHCSRCFILE)
	
	@echo "Building Windows AMD64..."
	GOARCH=amd64 GOOS=windows $(CLIENTBUILD) -ldflags "$(LDFLAGS) -H windowsgui" -o $(BUILDDIR)/windows/JumpServerClient.exe $(CLIENTSRCFILE)
	
	@echo "Building Windows 386..."
	GOARCH=386 GOOS=windows $(CLIENTBUILD) -ldflags "$(LDFLAGS) -H windowsgui" -o $(BUILDDIR)/windows/JumpServerClient32.exe $(CLIENTSRCFILE)
	
	@echo "Copying platform-specific resources..."
	@cp -R $(BASEPATH)/Scripts $(BUILDDIR)/darwin-amd64/
	@cp -R $(BASEPATH)/Scripts $(BUILDDIR)/darwin-arm64/
	@cp $(BASEPATH)/putty.exe $(BUILDDIR)/windows/
	@cp $(BASEPATH)/pkg/autoit/*.dll $(BUILDDIR)/windows/
	
	@echo "Copying to Tauri resources..."
	@mkdir -p $(BASEPATH)/../src-tauri/resources/bin/
	@cp -R $(BUILDDIR)/* $(BASEPATH)/../src-tauri/resources/bin/
	@cp $(BASEPATH)/config.json $(BASEPATH)/../src-tauri/resources/bin/
	
	@echo "Build completed for all platforms!"

# 帮助信息
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make           - Build for current platform only (default)"
	@echo "  make build-client - Build for current platform only"
	@echo "  make build-all - Build for all platforms (darwin, windows, linux)"
	@echo "  make help      - Show this help message"
	@echo ""
	@echo "Current platform: $(CURRENT_OS)/$(CURRENT_ARCH)"

# build-client 就是 build-current
.PHONY: build-client
build-client: build-current
