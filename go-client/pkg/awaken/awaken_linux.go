package awaken

import (
	"fmt"
	"go-client/global"
	"go-client/pkg/config"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

func getCommandFromArgs(connectInfo map[string]string, argFormat string) string {
	for key, value := range connectInfo {
		argFormat = strings.Replace(argFormat, "{"+key+"}", value, 1)
	}
	return argFormat
}

// validateAppPath checks if the application path exists
func validateAppPath(appPath string) error {
	if appPath == "" {
		return fmt.Errorf("application path is empty")
	}
	// Check if path exists
	if _, err := os.Stat(appPath); os.IsNotExist(err) {
		return fmt.Errorf("application path does not exist: %s", appPath)
	}
	return nil
}

// terminalCommand picks an installed terminal emulator for the current desktop.
// Distributions such as Kylin V10 report UKUI but only ship mate-terminal.
func terminalCommand(command string) *exec.Cmd {
	shell := fmt.Sprintf("%s; exec bash -i", command)
	desktop := strings.ToLower(strings.TrimSpace(os.Getenv("XDG_CURRENT_DESKTOP")))
	if desktop == "" {
		desktop = strings.ToLower(strings.TrimSpace(os.Getenv("DESKTOP_SESSION")))
	}

	var candidates []string
	switch {
	case strings.Contains(desktop, "ukui"):
		candidates = []string{"ukui-terminal", "mate-terminal", "gnome-terminal", "x-terminal-emulator"}
	case strings.Contains(desktop, "deepin"):
		candidates = []string{"deepin-terminal", "mate-terminal", "x-terminal-emulator"}
	case strings.Contains(desktop, "kde"):
		candidates = []string{"konsole", "x-terminal-emulator"}
	case strings.Contains(desktop, "xfce"):
		candidates = []string{"xfce4-terminal", "x-terminal-emulator"}
	case strings.Contains(desktop, "lxde"):
		candidates = []string{"lxterminal", "x-terminal-emulator"}
	case strings.Contains(desktop, "gnome"), strings.Contains(desktop, "cinnamon"), strings.Contains(desktop, "unity"):
		candidates = []string{"gnome-terminal", "mate-terminal", "x-terminal-emulator"}
	default:
		candidates = []string{"gnome-terminal", "mate-terminal", "ukui-terminal", "deepin-terminal", "konsole", "xfce4-terminal", "lxterminal", "x-terminal-emulator"}
	}

	for _, name := range candidates {
		if _, err := exec.LookPath(name); err != nil {
			continue
		}
		switch name {
		case "gnome-terminal":
			return exec.Command(name, "--", "bash", "-c", shell)
		case "deepin-terminal":
			return exec.Command(name, "--keep-open", "-C", shell)
		case "konsole":
			return exec.Command(name, "--noclose", "-e", "bash", "-c", shell)
		case "xfce4-terminal":
			return exec.Command(name, "--hold", "-e", "bash", "-c", shell)
		default:
			return exec.Command(name, "-x", "bash", "-c", shell)
		}
	}
	return nil
}

func awakenRDPCommand(filePath string, cfg *config.AppConfig) *exec.Cmd {
	global.LOG.Debug(filePath)
	var appItem *config.AppItem
	appLst := cfg.Linux.RemoteDesktop
	for _, app := range appLst {
		if app.IsMatchProtocol("rdp") {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	args := strings.Replace(appItem.ArgFormat, "{file}", filePath, 1)
	cmd := exec.Command(appItem.Name, strings.Split(args, " ")...)
	return cmd
}

func awakenVNCCommand(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	appLst := cfg.Linux.RemoteDesktop
	for _, app := range appLst {
		if app.IsMatchProtocol("vnc") {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	connectMap := map[string]string{
		"name":     r.getName(),
		"protocol": r.Protocol,
		"username": r.getUserName(),
		"value":    r.Value,
		"host":     r.Host,
		"port":     strconv.Itoa(r.Port),
	}

	if !appItem.IsInternal {
		if err := validateAppPath(appItem.Path); err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
	}
	commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
	cmd := exec.Command(appItem.Path, strings.Split(commands, " ")...)
	// 设置环境变量（只对这个子进程有效）
	cmd.Env = append(os.Environ(),
		"VNC_USERNAME="+r.getUserName(),
		"VNC_PASSWORD="+r.Value,
	)
	return cmd
}

func awakenSSHCommand(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	var appLst []config.AppItem
	switch r.Protocol {
	case "ssh", "telnet":
		appLst = cfg.Linux.Terminal
	case "sftp":
		appLst = cfg.Linux.FileTransfer
	}

	for _, app := range appLst {
		if app.IsMatchProtocol(r.Protocol) {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}

	// telnet 协议使用 ssh 的配置参数格式
	protocol := r.Protocol
	if protocol == "telnet" {
		protocol = "ssh"
	}

	var cmd *exec.Cmd
	connectMap := map[string]string{
		"name":     r.getName(),
		"protocol": protocol,
		"username": r.getUserName(),
		"value":    r.Value,
		"host":     r.Host,
		"port":     strconv.Itoa(r.Port),
	}

	if appItem.IsInternal {
		currentPath, _ := filepath.Abs(filepath.Dir(os.Args[0]))
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		clientPath := filepath.Join(currentPath, "client")
		cmd = terminalCommand(fmt.Sprintf("%s %s", clientPath, commands))
		if cmd == nil {
			global.LOG.Error("No supported terminal emulator found")
		}
	} else {
		if r.Protocol == "sqlserver" {
			connectMap["protocol"] = "mssql_jdbc_ms_new"
		}
		appPath := appItem.Path
		if !appItem.IsInternal {
			if err := validateAppPath(appItem.Path); err != nil {
				global.LOG.Error(err.Error())
				return nil
			}
		}
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		cmd = exec.Command(appPath, strings.Split(commands, " ")...)
	}
	return cmd
}

func awakenDBCommand(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	appLst := cfg.Linux.Databases
	for _, app := range appLst {
		if app.IsMatchProtocol(r.Protocol) {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	var cmd *exec.Cmd
	connectMap := map[string]string{
		"name":     r.getName(),
		"protocol": r.Protocol,
		"username": r.getUserName(),
		"value":    r.Value,
		"host":     r.Host,
		"port":     strconv.Itoa(r.Port),
		"dbname":   r.DBName,
	}
	if r.Protocol == "oracle" {
		connectMap["dbname"] = r.getUserName()
	}
	applyDBXProtocol(connectMap, appItem.Name, r.Protocol)
	if appItem.IsInternal {
		var argFormat string
		switch r.Protocol {
		case "redis":
			argFormat = "redis-cli -h {host} -p {port} -a {username}@{value}"
		case "oracle":
			argFormat = "sqlplus {username}/{value}@{host}:{port}/{dbname}"
		case "postgresql":
			argFormat = "psql 'user={username} password={value} host={host} dbname={dbname} port={port}'"
		case "mysql", "mariadb":
			argFormat = "mysql -u {username} -p{value} -h {host} -P {port} {dbname}"
		case "sqlserver":
			argFormat = "sqlcmd -S {host},{port} -U {username} -P {value} -d {dbname}"
		case "mongodb":
			argFormat = "mongosh mongodb://{username}:{value}@{host}:{port}/{dbname}"
		}
		commands := getCommandFromArgs(connectMap, argFormat)

		cmd = terminalCommand(commands)
		if cmd == nil {
			global.LOG.Error("No supported terminal emulator found")
		}
		return cmd
	} else {
		appPath := appItem.Path
		if !appItem.IsInternal {
			if err := validateAppPath(appItem.Path); err != nil {
				global.LOG.Error(err.Error())
				return nil
			}
		}
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		return exec.Command(appPath, strings.Split(commands, " ")...)
	}
}

func awakenOtherCommand(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	cmd := new(exec.Cmd)
	return cmd
}
