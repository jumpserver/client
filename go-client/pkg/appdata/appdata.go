package appdata

import (
	"os"
	"path/filepath"
	"strings"
)

const directoryEnv = "CLIENT_APP_DATA_NAME"

// DirectoryName returns the per-application directory passed by the Qt client.
// The legacy name remains as a fallback for direct launcher invocations.
func DirectoryName() string {
	name := strings.TrimSpace(os.Getenv(directoryEnv))
	if name == "" {
		return "jumpserver-client"
	}
	for _, character := range name {
		if character == '.' || character == '-' || character == '_' ||
			character >= '0' && character <= '9' ||
			character >= 'a' && character <= 'z' ||
			character >= 'A' && character <= 'Z' {
			continue
		}
		return "jumpserver-client"
	}
	if name == "." || name == ".." {
		return "jumpserver-client"
	}
	return name
}

func Directory(parts ...string) (string, error) {
	root, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(append([]string{root, DirectoryName()}, parts...)...), nil
}
