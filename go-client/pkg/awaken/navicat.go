package awaken

import (
	"net/url"
	"regexp"
	"strings"
)

// Magnus routes MariaDB through its server-first MySQL wire listener. Navicat's
// MariaDB-only profile rejects that greeting before it can use the asset token.
// The MySQL profile supports the shared wire protocol; the asset identity and
// audit protocol remain MariaDB.
func navicatWireProtocol(protocol string) string {
	if strings.EqualFold(protocol, "mariadb") || strings.EqualFold(protocol, "maria") {
		return "mysql"
	}
	return protocol
}

func getNavicatURL(connectInfo map[string]string) string {
	re := regexp.MustCompile(`@(.+)$`)
	matches := re.FindStringSubmatch(connectInfo["name"])
	name := connectInfo["username"]
	if len(matches) > 1 {
		name = matches[1]
	}
	query := url.Values{
		"Conn.Host":     {connectInfo["host"]},
		"Conn.Name":     {name},
		"Conn.Port":     {connectInfo["port"]},
		"Conn.Username": {connectInfo["username"]},
		"Conn.Password": {connectInfo["value"]},
	}
	protocol := navicatWireProtocol(connectInfo["protocol"])
	switch protocol {
	case "oracle":
		protocol = "ora"
		query.Set("Conn.ServiceName", connectInfo["dbname"])
		query.Set("Conn.ServiceNameType", "ServiceName")
		query.Set("Conn.ConnectionMode", "Basic")
	case "sqlserver":
		protocol = "mssql"
		query.Set("Conn.AuthenticationType", "Default")
		query.Set("Conn.InitialDatabase", connectInfo["dbname"])
	case "postgresql":
		protocol = "pgsql"
		query.Set("Conn.InitialDatabase", connectInfo["dbname"])
	}
	return "navicat://conn." + protocol + "?" + query.Encode()
}
