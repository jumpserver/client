// +build darwin linux

package main

import (
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"
	gossh "golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/terminal"
)

var (
	supportedCiphers = []string{
		"aes128-ctr", "aes192-ctr", "aes256-ctr",
		"aes128-gcm@openssh.com",
		"chacha20-poly1305@openssh.com",
		"arcfour256", "arcfour128", "arcfour",
		"aes128-cbc", "3des-cbc"}

	supportedKexAlgos = []string{
		"diffie-hellman-group1-sha1",
		"diffie-hellman-group14-sha1", "ecdh-sha2-nistp256", "ecdh-sha2-nistp521",
		"ecdh-sha2-nistp384", "curve25519-sha256@libssh.org"}

	supportedHostKeyAlgos = []string{
		"ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
		"ssh-rsa", "ssh-dss", "ssh-ed25519"}
)

func WatchWindowSize(sigwinchCh chan os.Signal) {
	signal.Notify(sigwinchCh, syscall.SIGWINCH)
}

// sshCmd represents the ssh command
var sshCmd = &cobra.Command{
	Use:   "ssh",
	Short: "JMS KoKo ssh debug tool",
	Long: `JMS KoKo ssh tool
For example:
jmstool ssh root@127.0.0.1 -p 2222
`,
	Run: func(cmd *cobra.Command, args []string) {
		var (
			username    = ""
			host        = ""
			port        = "2222"
			privateFile = ""
			password    = ""
		)
		for i := range args {
			if strings.Contains(args[i], "@") {
				usernameHost := strings.Split(args[i], "@")
				if len(usernameHost) != 2 {
					fmt.Println("error format: ", args[i])
					os.Exit(1)
				}
				username, host = usernameHost[0], usernameHost[1]
				break
			}
		}
		if username == "" || host == "" {
			_ = cmd.Help()
			os.Exit(1)
		}
		auths := make([]gossh.AuthMethod, 0, 2)

		if flagPort, err := cmd.PersistentFlags().GetString("port"); err == nil {
			port = flagPort
		}
		if flagIdentity, err := cmd.PersistentFlags().GetString("identity"); err == nil {
			privateFile = flagIdentity
		}
		if flagPassword, err := cmd.PersistentFlags().GetString("password"); err == nil {
			password = flagPassword
		}
		if password != "" {
			auths = append(auths, gossh.Password(password))
		}
		if password == "" && privateFile == "" {
			if _, err := fmt.Fprintf(os.Stdout, "%s@%s password: ", username, host); err != nil {
				log.Fatal(err)
			}
			if result, err := terminal.ReadPassword(int(os.Stdout.Fd())); err != nil {
				log.Fatal(err)
			} else {
				auths = append(auths, gossh.Password(string(result)))
			}
		}
		if privateFile != "" {
			raw, err := ioutil.ReadFile(privateFile)
			if err != nil {
				log.Fatal(err)
			}
			signer, err := gossh.ParsePrivateKey(raw)
			if err != nil {
				log.Fatal(err)
			}

			auths = append(auths, gossh.PublicKeys(signer))
		}
		config := &gossh.ClientConfig{
			User:              username,
			Auth:              auths,
			HostKeyCallback:   gossh.InsecureIgnoreHostKey(),
			Config:            gossh.Config{Ciphers: supportedCiphers, KeyExchanges: supportedKexAlgos},
			Timeout:           30 * time.Second,
			HostKeyAlgorithms: supportedHostKeyAlgos,
		}
		client, err := gossh.Dial("tcp", net.JoinHostPort(host, port), config)
		if err != nil {
			log.Fatalf("dial err: %s", err)
		}
		defer client.Close()
		sess, err := client.NewSession()
		if err != nil {
			log.Fatalf("Session err: %s", err)
		}
		modes := gossh.TerminalModes{
			gossh.ECHO:          1,     // enable echoing
			gossh.TTY_OP_ISPEED: 14400, // input speed = 14.4 kbaud
			gossh.TTY_OP_OSPEED: 14400, // output speed = 14.4 kbaud
		}
		xterm := os.Getenv("xterm")
		if xterm == "" {
			xterm = "xterm-256color"
		}
		fd := int(os.Stdin.Fd())
		w, h, _ := terminal.GetSize(fd)
		err = sess.RequestPty(xterm, h, w, modes)
		if err != nil {
			log.Fatalf("RequestPty err: %s", err)
		}
		in, err := sess.StdinPipe()
		if err != nil {
			log.Fatalf("StdinPipe err: %s", err)
		}
		out, err := sess.StdoutPipe()
		if err != nil {
			log.Fatalf("StdoutPipe err: %s", err)
		}
		state, err := terminal.MakeRaw(fd)
		if err != nil {
			log.Fatalf("MakeRaw err: %s", err)
		}
		defer terminal.Restore(fd, state)

		go io.Copy(in, os.Stdin)
		go io.Copy(os.Stdout, out)
		sigwinchCh := make(chan os.Signal, 1)
		WatchWindowSize(sigwinchCh)
		sigChan := make(chan struct{}, 1)
		err = sess.Shell()
		if err != nil {
			log.Fatalf("Shell err: %s", err)
		}
		go func() {
			for {
				select {
				case <-sigChan:
					return

				// 阻塞读取
				case sigwinch := <-sigwinchCh:
					if sigwinch == nil {
						return
					}
					w, h, err := terminal.GetSize(fd)
					if err != nil {
						log.Printf("Unable to send window-change reqest: %s. \n", err)
						continue
					}
					if err := sess.WindowChange(h, w); err != nil {
						log.Println("Window change err: ", err)
					}
				}
			}
		}()
		err = sess.Wait()
		sigChan <- struct{}{}
		if err != nil {
			log.Fatalf("Wait err: %s", err)
		}
	},
}

func init() {
	sshCmd.PersistentFlags().StringP("port", "p", "22", "ssh port")
	sshCmd.PersistentFlags().StringP("password", "P", "", "ssh password")
	sshCmd.PersistentFlags().StringP("identity", "i", "", "identity_file")
}

func main() {
	if err := sshCmd.Execute(); err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
