package awaken

import (
	"go-client/global"
	"go-client/pkg/config"
	"go.uber.org/zap"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestRemoteAppRDPLaunch(t *testing.T) {
	oldLog := global.LOG
	global.LOG = zap.NewNop()
	t.Cleanup(func() { global.LOG = oldLog })
	dir := t.TempDir()
	// Substitute only LaunchServices' command for a recording executable.
	// No user config files or real remote sessions are touched.
	openPath := filepath.Join(dir, "open")
	if err := os.WriteFile(openPath, []byte("#!/bin/sh\nprintf '%s' \"$1\"\n"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	rdpPath := filepath.Join(dir, "remote app.rdp")
	if err := os.WriteFile(rdpPath, []byte("remoteapplicationmode:i:1\r\nremoteapplicationprogram:s:||chrome\r\n"), 0600); err != nil {
		t.Fatal(err)
	}
	cmd := awakenRDPCommand(rdpPath, &config.AppConfig{})
	output, err := cmd.Output()
	if err != nil || string(output) != rdpPath {
		t.Fatal("RemoteApp file was not passed intact to the RDP file association")
	}
}

func testVNCBundle(t *testing.T) (string, string) {
	t.Helper()
	bundle := filepath.Join(t.TempDir(), "Tiger VNC.app")
	binary := filepath.Join(bundle, "Contents", "MacOS", "viewer executable")
	if err := os.MkdirAll(filepath.Dir(binary), 0700); err != nil {
		t.Fatal(err)
	}
	plist := `<?xml version="1.0"?><plist version="1.0"><dict><key>CFBundleExecutable</key><string>viewer executable</string></dict></plist>`
	if err := os.WriteFile(filepath.Join(bundle, "Contents", "Info.plist"), []byte(plist), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(binary, []byte("#!/bin/sh\nprintf '%s\\n' \"$@\" \"$VNC_USERNAME\" \"$VNC_PASSWORD\"\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return bundle, binary
}

func TestVNCBundleAndBinaryLaunch(t *testing.T) {
	for _, kind := range []string{"xml-bundle", "binary-plist-bundle", "direct-binary"} {
		t.Run(kind, func(t *testing.T) {
			bundle, binary := testVNCBundle(t)
			path := bundle
			if kind == "binary-plist-bundle" {
				if err := exec.Command("/usr/bin/plutil", "-convert", "binary1", filepath.Join(bundle, "Contents", "Info.plist")).Run(); err != nil {
					t.Fatal(err)
				}
			} else if kind == "direct-binary" {
				path = binary
			}
			cfg := config.AppConfig{MacOS: config.AppType{RemoteDesktop: []config.AppItem{{
				Name: "tigervnc", Path: path, MatchFirst: []string{"vnc"},
				ArgFormat: "--Shared=on --RemoteResize=on --FullScreen=off {host}::{port}",
			}}}}
			r := &Rouse{Info: Info{Protocol: "vnc", Endpoint: Endpoint{Host: "127.0.0.1", Port: 5999},
				Token: Token{ID: "synthetic-user", Value: "synthetic value $ ' &"}}}
			cmd := awakenVNCCommand(r, &cfg)
			if cmd == nil || cmd.Path != binary {
				t.Fatal("bundle did not resolve to its declared executable")
			}
			output, err := cmd.Output()
			if err != nil {
				t.Fatal(err)
			}
			want := "--Shared=on\n--RemoteResize=on\n--FullScreen=off\n127.0.0.1::5999\nsynthetic-user\nsynthetic value $ ' &\n"
			if string(output) != want {
				t.Fatal("VNC arguments or child environment changed")
			}
		})
	}
}

func TestResolveMacExecutableRejectsInvalidPaths(t *testing.T) {
	for _, kind := range []string{"missing-path", "directory", "missing-plist", "missing-binary", "not-executable", "traversal"} {
		t.Run(kind, func(t *testing.T) {
			bundle, binary := testVNCBundle(t)
			path := bundle
			switch kind {
			case "missing-path":
				path += ".missing"
			case "directory":
				path = filepath.Dir(bundle)
			case "missing-plist":
				os.Remove(filepath.Join(bundle, "Contents", "Info.plist"))
			case "missing-binary":
				os.Remove(binary)
			case "not-executable":
				os.Chmod(binary, 0600)
			case "traversal":
				os.WriteFile(filepath.Join(bundle, "Contents", "Info.plist"), []byte(`<plist version="1.0"><dict><key>CFBundleExecutable</key><string>../outside</string></dict></plist>`), 0600)
			}
			if _, err := resolveMacExecutable(path); err == nil {
				t.Fatal("invalid application path accepted")
			}
		})
	}
}

func TestInstalledVNCBundleHelp(t *testing.T) {
	bundle := os.Getenv("OSM_TEST_VNC_APP")
	if bundle == "" {
		t.Skip("optional installed TigerVNC smoke test")
	}
	cfg := config.AppConfig{MacOS: config.AppType{RemoteDesktop: []config.AppItem{{
		Path: bundle, MatchFirst: []string{"vnc"}, ArgFormat: "--help",
	}}}}
	cmd := awakenVNCCommand(&Rouse{}, &cfg)
	if cmd == nil {
		t.Fatal("installed bundle was not resolved")
	}
	output, _ := cmd.CombinedOutput() // TigerVNC may return a nonzero status for help.
	if !strings.Contains(string(output), "TigerVNC") || !strings.Contains(string(output), "Usage:") {
		t.Fatal("installed viewer did not run its help command")
	}
}
