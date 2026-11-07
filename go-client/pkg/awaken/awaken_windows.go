package awaken

import (
	"encoding/json"
	"fmt"
	"go-client/global"
	"go-client/pkg/autoit"
	"go-client/pkg/config"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

func EnsureDirExist(path string) {
	if fi, err := os.Stat(path); err == nil && fi.IsDir() {
		return
	}
	if err := os.MkdirAll(path, os.ModePerm); err != nil {
		global.LOG.Error(err.Error())
	}
}

func getNavicatURL(connectInfo map[string]string) string {
	re := regexp.MustCompile(`@(.+)$`)
	matches := re.FindStringSubmatch(connectInfo["name"])
	name := connectInfo["username"]
	if len(matches) > 1 {
		name = matches[1]
	}
	url := fmt.Sprintf("navicat://conn.%s?Conn.Host=%s&Conn.Name=%s&Conn.Port=%s&Conn.Username=%s",
		connectInfo["protocol"], connectInfo["host"], name, connectInfo["port"], connectInfo["username"])
	switch connectInfo["protocol"] {
	case "oracle":
		url = strings.Replace(url, "conn.oracle", "conn.ora", 1)
		url += fmt.Sprintf("&Conn.ServiceName=%s&Conn.ServiceNameType=ServiceName&Conn.ConnectionMode=Basic", connectInfo["dbname"])
	case "sqlserver":
		url = strings.Replace(url, "conn.sqlserver", "conn.mssql", 1)
		url += fmt.Sprintf("&Conn.AuthenticationType=Default&Conn.InitialDatabase=%s", connectInfo["dbname"])
	case "postgresql":
		url = strings.Replace(url, "conn.postgresql", "conn.pgsql", 1)
		url += fmt.Sprintf("&Conn.InitialDatabase=%s", connectInfo["dbname"])
	}

	pattern := regexp.MustCompile(`[\^(){}~]`)
	url = pattern.ReplaceAllStringFunc(url, func(match string) string {
		return fmt.Sprintf("{%s}", match)
	})
	return url
}

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

func handleRDP(r *Rouse, filePath string, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	appLst := cfg.Windows.RemoteDesktop
	for _, app := range appLst {
		if app.IsMatchProtocol("rdp") {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	appPath := appItem.Path
	if !appItem.IsInternal {
		if err := validateAppPath(appItem.Path); err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
	}

	args := strings.Replace(appItem.ArgFormat, "{file}", filePath, 1)
	return exec.Command(appPath, args)
}

func handleVNC(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	appLst := cfg.Windows.RemoteDesktop
	for _, app := range appLst {
		if app.IsMatchProtocol("vnc") {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	if !appItem.IsInternal {
		if err := validateAppPath(appItem.Path); err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
	}
	connectMap := map[string]string{
		"name":     r.getName(),
		"protocol": r.Protocol,
		"username": r.getUserName(),
		"value":    r.Value,
		"host":     r.Host,
		"port":     strconv.Itoa(r.Port),
	}
	if appItem.Name == "realvnc" {
		return handleRealVNC(r, appItem)
	}
	if len(appItem.AutoIt) == 0 {
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		cmd := exec.Command(appItem.Path, strings.Split(commands, " ")...)
		// 设置环境变量（只对这个子进程有效）
		cmd.Env = append(os.Environ(),
			"VNC_USERNAME="+r.getUserName(),
			"VNC_PASSWORD="+r.Value,
		)
		return cmd
	} else {
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		global.LOG.Error(appItem.Path + " " + commands)
		autoit.LoadAuto()
		autoit.Run(appItem.Path + " " + commands)
		for _, item := range appItem.AutoIt {
			time.Sleep(300 * time.Millisecond)
			switch item.Cmd {
			case "Wait":
				sleepTime, _ := strconv.Atoi(item.Type)
				winTitle := item.Element
				maxRetry := 0
				for {
					ret := autoit.WinWaitActive(winTitle, "", sleepTime)
					time.Sleep(time.Duration(sleepTime) * 100 * time.Millisecond)
					if ret != 0 || maxRetry > 30 {
						break
					}
					maxRetry++
				}
			case "ControlSend":
				maxRetry := 0
				for {
					ret := autoit.ControlSend("", "", item.Element, getCommandFromArgs(connectMap, item.Type))
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "ControlSetText":
				maxRetry := 0
				for {
					ret := autoit.ControlSetText("", "", item.Element, getCommandFromArgs(connectMap, item.Type))
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "ControlClick":
				pos := strings.Split(item.Type, ",")
				x, _ := strconv.Atoi(pos[0])
				y, _ := strconv.Atoi(pos[1])
				maxRetry := 0
				for {
					ret := autoit.ControlClick("", "", item.Element, "left", 1, x, y)
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "SendKey":
				autoit.Send(item.Element)
			}
		}
		return exec.Command("")
	}
}

func handleRealVNC(r *Rouse, appItem *config.AppItem) *exec.Cmd {
	dir, err := os.UserConfigDir()
	if err != nil {
		reportErrorf("Failed to locate RealVNC config directory: %v", err)
		return nil
	}
	dir = filepath.Join(dir, "jumpserver-client")
	if err := os.MkdirAll(dir, 0700); err != nil {
		reportErrorf("Failed to create RealVNC config directory: %v", err)
		return nil
	}
	// A unique file avoids deleting another connection's configuration.
	file, err := os.CreateTemp(dir, "realvnc-*.vnc")
	if err != nil {
		reportErrorf("Failed to create RealVNC connection file: %v", err)
		return nil
	}
	defer os.Remove(file.Name())
	content := fmt.Sprintf("[Connection]\nHost=%s:%d\nWarnUnencrypted=0\nUserName=%s\n",
		r.Host, r.Port, r.getUserName())
	_, writeErr := file.WriteString(content)
	closeErr := file.Close()
	if writeErr != nil || closeErr != nil {
		reportError("Failed to write RealVNC connection file")
		return nil
	}

	cmd := exec.Command(appItem.Path, "-VerifyId", "0", file.Name())
	if err := cmd.Start(); err != nil {
		reportErrorf("Failed to start RealVNC Viewer: %v", err)
		return nil
	}
	defer cmd.Process.Release()
	autoit.LoadAuto()
	winTitle := "Authentication"
	active := false
	for i := 0; i <= 30; i++ {
		if autoit.WinWaitActive(winTitle, "", 1) != 0 {
			active = true
			break
		}
		autoit.WinActivate(winTitle, "")
		time.Sleep(300 * time.Millisecond)
	}
	if !active {
		reportError("Timed out waiting for RealVNC Authentication dialog")
		return nil
	}
	control := autoit.ControlGetFocus(winTitle, "")
	if control == "" {
		reportError("Could not locate RealVNC authentication control")
		return nil
	}
	text := strings.TrimSpace(autoit.ControlGetText(winTitle, "", control))
	username := strings.TrimSpace(r.getUserName())
	if username != "" && text == username {
		autoit.ControlSend(winTitle, "", control, "{TAB}")
		time.Sleep(300 * time.Millisecond)
		nextControl := autoit.ControlGetFocus(winTitle, "")
		if nextControl == "" || nextControl == control {
			reportError("Could not focus RealVNC password control")
			return nil
		}
		control = nextControl
	}
	// Address the control explicitly so a focus change cannot type the password
	// into another foreground application.
	if autoit.ControlSend(winTitle, "", control, "^a") == 0 ||
		autoit.ControlSend(winTitle, "", control, r.Value, 1) == 0 ||
		autoit.ControlSend(winTitle, "", control, "{ENTER}") == 0 {
		reportError("Failed to fill RealVNC authentication dialog")
		return nil
	}
	return exec.Command("cmd", "/C", "exit", "0")
}

func handleSSH(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	var appLst []config.AppItem
	switch r.Protocol {
	case "ssh", "telnet":
		appLst = cfg.Windows.Terminal
	case "sftp":
		appLst = cfg.Windows.FileTransfer
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

	var appPath string
	if appItem.IsInternal {
		currentPath, _ := filepath.Abs(filepath.Dir(os.Args[0]))
		appPath = filepath.Join(currentPath, appItem.Path)
	} else {
		appPath = appItem.Path
	}
	if !appItem.IsInternal {
		if err := validateAppPath(appItem.Path); err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
	}
	connectMap := map[string]string{
		"name":     r.getName(),
		"protocol": protocol,
		"username": r.getUserName(),
		"value":    r.Value,
		"host":     r.Host,
		"port":     strconv.Itoa(r.Port),
	}
	commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
	if strings.Contains(commands, "*") {
		commands := strings.Split(commands, "*")
		return exec.Command(appPath, commands[0], commands[1])
	} else {
		commands := strings.Split(commands, " ")
		return exec.Command(appPath, commands...)
	}
}

func handleDB(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	var appItem *config.AppItem
	appLst := cfg.Windows.Databases
	for _, app := range appLst {
		if app.IsMatchProtocol(r.Protocol) {
			appItem = &app
			break
		}
	}
	if appItem == nil {
		return nil
	}
	appPath := appItem.Path
	if !appItem.IsInternal {
		if err := validateAppPath(appItem.Path); err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
	}

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
	if r.Protocol == "sqlserver" && appItem.Name == "dbeaver" {
		connectMap["protocol"] = "mssql_jdbc_ms_new"
	}
	if r.Protocol == "redis" && appItem.Name == "resp" {
		var conList []map[string]string
		ss := make(map[string]string)
		ss["host"] = r.Host
		ss["port"] = strconv.Itoa(r.Port)
		ss["name"] = r.getName()
		ss["auth"] = r.Token.ID + "@" + r.Value
		ss["ssh_agent_path"] = ""
		ss["ssh_password"] = ""
		ss["ssh_private_key_path"] = ""
		ss["timeout_connect"] = "60000"
		ss["timeout_execute"] = "60000"
		conList = append(conList, ss)

		bjson, _ := json.Marshal(conList)
		dir, _ := os.UserConfigDir()
		currentPath := filepath.Join(dir, "jumpserver-client")
		rdmPath := filepath.Join(currentPath, ".rdm")
		EnsureDirExist(rdmPath)
		filePath := filepath.Join(rdmPath, "connections.json")
		global.LOG.Error(filePath)
		err := ioutil.WriteFile(filePath, bjson, os.ModePerm)
		if err != nil {
			global.LOG.Error(err.Error())
			return nil
		}
		connectMap["config_file"] = currentPath

	}
	if appItem.Name == "navicat17" {
		url := getNavicatURL(connectMap)
		connectMap["url"] = url
	}
	if len(appItem.AutoIt) == 0 {
		commands := getCommandFromArgs(connectMap, appItem.ArgFormat)
		if strings.Contains(commands, "*") {
			commands := strings.Split(commands, "*")
			return exec.Command(appPath, commands...)
		} else {
			commands := strings.Split(commands, " ")
			return exec.Command(appPath, commands...)
		}
	} else {
		autoit.LoadAuto()
		autoit.Run(appPath)
		for _, item := range appItem.AutoIt {
			time.Sleep(300 * time.Millisecond)
			switch item.Cmd {
			case "Wait":
				sleepTime, _ := strconv.Atoi(item.Type)
				winTitle := item.Element
				maxRetry := 0
				for {
					ret := autoit.WinWaitActive(winTitle, "", sleepTime)
					time.Sleep(time.Duration(sleepTime) * 100 * time.Millisecond)
					if ret != 0 || maxRetry > 30 {
						break
					}
					maxRetry++
				}
			case "ControlSend":
				maxRetry := 0
				for {
					ret := autoit.ControlSend("", "", item.Element, getCommandFromArgs(connectMap, item.Type))
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "ControlSetText":
				maxRetry := 0
				for {
					ret := autoit.ControlSetText("", "", item.Element, getCommandFromArgs(connectMap, item.Type))
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "ControlClick":
				pos := strings.Split(item.Type, ",")
				x, _ := strconv.Atoi(pos[0])
				y, _ := strconv.Atoi(pos[1])
				maxRetry := 0
				for {
					ret := autoit.ControlClick("", "", item.Element, "left", 1, x, y)
					time.Sleep(300 * time.Millisecond)
					if ret != 0 || maxRetry > 10 {
						break
					}
					maxRetry++
				}
			case "SendKey":
				autoit.Send(item.Element)
			}
		}
		return exec.Command("")
	}
}

func handleCommand(r *Rouse, cfg *config.AppConfig) *exec.Cmd {
	cmd := exec.Command(r.Command)
	return cmd
}
