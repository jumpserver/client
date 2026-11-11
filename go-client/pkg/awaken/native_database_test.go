package awaken

import (
	"encoding/xml"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func nativeInfo(protocol string) *Rouse {
	return &Rouse{Info: Info{
		Protocol: protocol, Name: "Native test",
		Token:    Token{ID: "selector-token", Value: " secret ' &+=中文 "},
		Endpoint: Endpoint{Host: "gateway.example.com", Port: 15525},
		Asset:    Asset{DBInfo: DBInfo{DBName: "ANALYTICS"}},
	}}
}

func TestNativeDatabaseArgs(t *testing.T) {
	for _, protocol := range []string{"db2", "clickhouse"} {
		t.Run(protocol, func(t *testing.T) {
			directory := t.TempDir()
			info := nativeInfo(protocol)
			args, err := nativeDatabaseArgs(info, directory)
			if err != nil {
				t.Fatal(err)
			}
			driver := protocol
			suffix := "|prop.securityMechanism=3"
			if protocol == "clickhouse" {
				driver, suffix = "jumpserver-clickhouse-native", ""
				workspace := filepath.Join(directory, "dbeaver-clickhouse-native")
				if len(args) != 4 || args[0] != "-data" || args[1] != workspace {
					t.Fatal("wrong isolated workspace arguments")
				}
				definition, err := os.ReadFile(filepath.Join(workspace, ".metadata", ".config", "drivers.xml"))
				if err != nil {
					t.Fatal(err)
				}
				if strings.Contains(string(definition), info.Value) || strings.Contains(string(definition), info.Token.ID) {
					t.Fatal("persisted token in driver configuration")
				}
				var document struct{ XMLName xml.Name }
				if err := xml.Unmarshal(definition, &document); err != nil || document.XMLName.Local != "drivers" {
					t.Fatal("invalid driver XML")
				}
				args = args[2:]
			}
			if len(args) != 2 || args[0] != "-con" {
				t.Fatal("wrong DBeaver argument boundaries")
			}
			expected := "name=Nativetest|driver=" + driver + "|user=selector-token|password= secret ' &+=中文 |database=ANALYTICS|host=gateway.example.com|port=15525|save=false|connect=true" + suffix
			if args[1] != expected {
				t.Fatal("connection fields were changed or split")
			}
			if info.Protocol != protocol || info.DBName != "ANALYTICS" {
				t.Fatal("mutated connection data")
			}
		})
	}
}

func TestNativeDatabaseRejectsDBeaverDelimiters(t *testing.T) {
	for _, protocol := range []string{"db2", "clickhouse"} {
		for _, value := range []string{"a|prop.securityMechanism=9", "a\x00b"} {
			info := nativeInfo(protocol)
			info.Value = value
			if _, err := nativeDatabaseArgs(info, t.TempDir()); err == nil || strings.Contains(err.Error(), value) {
				t.Fatal("unsafe delimiter accepted or secret leaked")
			}
		}
	}
	if _, err := nativeDatabaseArgs(nativeInfo("mysql"), t.TempDir()); err == nil {
		t.Fatal("changed existing protocol routing")
	}
}

func TestNativeWorkspaceInitializationIsAtomicAndPreservesExistingDrivers(t *testing.T) {
	directory := t.TempDir()
	var group sync.WaitGroup
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			if _, err := prepareClickHouseWorkspace(directory); err != nil {
				t.Error(err)
			}
		}()
	}
	group.Wait()
	target := filepath.Join(directory, "dbeaver-clickhouse-native", ".metadata", ".config", "drivers.xml")
	data, err := os.ReadFile(target)
	if err != nil || string(data) != clickHouseNativeDriver {
		t.Fatal("incomplete driver configuration")
	}
	const existing = "<drivers><!-- user configuration --></drivers>"
	if err := os.WriteFile(target, []byte(existing), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := prepareClickHouseWorkspace(directory); err != nil {
		t.Fatal(err)
	}
	data, _ = os.ReadFile(target)
	if string(data) != existing {
		t.Fatal("overwrote an existing DBeaver workspace")
	}
	files, _ := filepath.Glob(filepath.Join(filepath.Dir(target), "*.tmp"))
	if len(files) != 0 {
		t.Fatal("left temporary driver files behind")
	}
}
