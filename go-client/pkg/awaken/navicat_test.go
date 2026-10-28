package awaken

import (
	"net/url"
	"strings"
	"testing"
)

func TestNavicatSharedMySQLWireProfile(t *testing.T) {
	for _, protocol := range []string{"mariadb", "maria", "mysql", "oracle", "sqlserver", "postgresql"} {
		t.Run(protocol, func(t *testing.T) {
			info := map[string]string{"protocol": protocol, "host": "proxy.example", "port": "5528", "username": "asset-token", "name": "root@db asset", "dbname": "app"}
			url := getNavicatURL(info)
			want := map[string]string{"mariadb": "mysql", "maria": "mysql", "mysql": "mysql", "oracle": "ora", "sqlserver": "mssql", "postgresql": "pgsql"}[protocol]
			if !strings.HasPrefix(url, "navicat://conn."+want+"?") {
				t.Fatalf("URL = %q, want Navicat profile %q", url, want)
			}
			for _, part := range []string{"Conn.Host=proxy.example", "Conn.Port=5528", "Conn.Username=asset-token"} {
				if !strings.Contains(url, part) {
					t.Fatalf("URL %q lost %q", url, part)
				}
			}
			if info["protocol"] != protocol {
				t.Fatal("Navicat URL generation changed the asset protocol")
			}
		})
	}
}

func TestNavicatURICredentials(t *testing.T) {
	info := map[string]string{
		"protocol": "sqlserver", "host": "proxy.example", "port": "5528",
		"name": "root@db asset", "username": "token &+",
		"value": "secret &+=?#% ^(){}~*\" 中文", "dbname": "db &+ 中文",
	}
	u, err := url.Parse(getNavicatURL(info))
	if err != nil {
		t.Fatal(err)
	}
	query := u.Query()
	for key, want := range map[string]string{
		"Conn.Name": "db asset", "Conn.Username": info["username"],
		"Conn.Password": info["value"], "Conn.InitialDatabase": info["dbname"],
		"Conn.AuthenticationType": "Default",
	} {
		if len(query[key]) != 1 || query.Get(key) != want {
			t.Errorf("%s did not round trip", key)
		}
	}
	if u.Fragment != "" {
		t.Fatal("URI contains an unexpected fragment")
	}
}
