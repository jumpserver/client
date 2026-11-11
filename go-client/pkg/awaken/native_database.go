package awaken

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"go-client/pkg/config"
)

// This definition is intentionally separate from DBeaver's HTTP ClickHouse driver.
// Keep it in sync with the Electron plugin in jumpserver/luna.
const clickHouseNativeDriver = `<?xml version="1.0" encoding="UTF-8"?>
<drivers>
  <provider id="generic">
    <driver id="jumpserver-clickhouse-native" name="ClickHouse Native (JumpServer)"
            class="com.github.housepower.jdbc.ClickHouseDriver"
            url="jdbc:clickhouse://{host}:{port}[/{database}]" port="9000"
            custom="true" categories="sql,analytic" description="Magnus Native TCP (not HTTP)">
      <library type="jar" path="maven:/com.github.housepower:clickhouse-native-jdbc-shaded:2.5.6" custom="true"/>
    </driver>
  </provider>
</drivers>
`

func prepareClickHouseWorkspace(configDir string) (string, error) {
	workspace := filepath.Join(configDir, "dbeaver-clickhouse-native")
	directory := filepath.Join(workspace, ".metadata", ".config")
	if err := os.MkdirAll(directory, 0700); err != nil {
		return "", err
	}
	temporary, err := os.CreateTemp(directory, "drivers-*.tmp")
	if err != nil {
		return "", err
	}
	defer os.Remove(temporary.Name())
	if _, err = temporary.WriteString(clickHouseNativeDriver); err != nil {
		temporary.Close()
		return "", err
	}
	if err = temporary.Close(); err != nil {
		return "", err
	}
	// Publish a complete file and never overwrite an existing/running workspace.
	if err = os.Link(temporary.Name(), filepath.Join(directory, "drivers.xml")); err != nil && !os.IsExist(err) {
		return "", err
	}
	return workspace, nil
}

func nativeDatabaseArgs(r *Rouse, configDir string) ([]string, error) {
	fields := [][2]string{
		{"name", r.getName()}, {"driver", r.Protocol}, {"user", r.getUserName()},
		{"password", r.Value}, {"database", r.DBName},
		{"host", r.Host}, {"port", strconv.Itoa(r.Port)},
		{"save", "false"}, {"connect", "true"},
	}
	if r.Protocol == "db2" {
		fields = append(fields, [2]string{"prop.securityMechanism", "3"})
	} else if r.Protocol == "clickhouse" {
		fields[1][1] = "jumpserver-clickhouse-native"
	} else {
		return nil, fmt.Errorf("unsupported native database protocol")
	}
	parts := make([]string, 0, len(fields))
	for _, field := range fields {
		if strings.ContainsAny(field[1], "|\x00") {
			return nil, fmt.Errorf("DBeaver connection values contain a reserved delimiter")
		}
		parts = append(parts, field[0]+"="+field[1])
	}
	args := []string{"-con", strings.Join(parts, "|")}
	if r.Protocol == "clickhouse" {
		workspace, err := prepareClickHouseWorkspace(configDir)
		if err != nil {
			return nil, fmt.Errorf("cannot prepare ClickHouse Native workspace: %w", err)
		}
		args = append([]string{"-data", workspace}, args...)
	}
	return args, nil
}

func nativeDatabaseCommand(r *Rouse, app *config.AppItem) (*exec.Cmd, error) {
	if err := validateAppPath(app.Path); err != nil {
		return nil, err
	}
	directory, err := os.UserConfigDir()
	if err != nil {
		return nil, err
	}
	args, err := nativeDatabaseArgs(r, filepath.Join(directory, "jumpserver-client"))
	if err != nil {
		return nil, err
	}
	// Never split on spaces or use a shell: names, databases and passwords are data.
	return exec.Command(app.Path, args...), nil
}
