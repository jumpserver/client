package main

import (
	"encoding/base64"
	"encoding/json"
	"go-client/global"
	"go-client/pkg/awaken"
	_ "go-client/pkg/plugin"
	"go-client/pkg/logger"
	"os"
	"strings"

	"go.uber.org/zap"
)

// deepLinkSchemes 是支持的 deep link scheme 列表。
//
// JumpServer 服务端 v5 起下发的连接链接改用 jms2://，jms:// 是历史遗留值；
// 两者都接受，避免客户端升级后无法唤醒旧服务端下发的链接。
var deepLinkSchemes = []string{"jms2://", "jms://"}

// trimDeepLinkScheme 去掉 deep link 前缀，返回 base64 负载。
func trimDeepLinkScheme(arg string) string {
	for _, scheme := range deepLinkSchemes {
		if strings.HasPrefix(arg, scheme) {
			return strings.TrimPrefix(arg, scheme)
		}
	}
	return arg
}

func main() {
	p := &awaken.Info{}
	if len(os.Args) != 1 {
		global.LOG = logger.InitLogger()
		defer global.LOG.Sync()
		zap.ReplaceGlobals(global.LOG)
		base64String := trimDeepLinkScheme(os.Args[1])
		decoded, _ := base64.StdEncoding.DecodeString(base64String)
		infoJson := string(decoded)
		json.Unmarshal([]byte(infoJson), p)
		r := awaken.Rouse{
			Info: *p,
		}
		r.Run()
	}
}

//go build -o JumpServerClient awaken.go
//CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -ldflags="-H windowsgui" -o JumpServerClient.exe awaken.go
//CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o JumpServerClient awaken.go
