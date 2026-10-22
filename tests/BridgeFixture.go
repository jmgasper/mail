// This integration test is overlaid into Proton Bridge's internal/bridge test
// package. The Bridge checkout and its production sources remain unchanged.
package bridge_test

import (
    "context"
    "crypto/tls"
    "crypto/x509"
    "encoding/json"
    "fmt"
    "io"
    "net/mail"
    "os"
    "os/exec"
    "path/filepath"
    "strings"
    "testing"
    "time"

    "github.com/ProtonMail/go-proton-api"
    "github.com/ProtonMail/go-proton-api/server"
    "github.com/ProtonMail/proton-bridge/v3/internal/bridge"
    "github.com/ProtonMail/proton-bridge/v3/internal/events"
    "github.com/emersion/go-imap"
    "github.com/emersion/go-imap/client"
    "github.com/sirupsen/logrus"
    "github.com/stretchr/testify/require"
)

func TestMailInterop(t *testing.T) {
    executable := os.Getenv("MAIL_BRIDGE_CLIENT")
    if executable == "" { t.Skip("MAIL_BRIDGE_CLIENT selects the native Mail integration client") }
    previousLevel := logrus.GetLevel()
    logrus.SetLevel(logrus.ErrorLevel)
    defer logrus.SetLevel(previousLevel)
    require.Empty(t, os.Getenv("FEATURE_TEST_HOST_URL"), "this test must never use a live Proton service")
    withEnv(t, func(ctx context.Context, api *server.Server, ctl *proton.NetCtl, locator bridge.Locator, key []byte) {
        _, _, err := api.CreateUser("mailrecipient", password)
        require.NoError(t, err)
        _, _, err = api.CreateUser("mailhidden", password)
        require.NoError(t, err)
        directory := t.TempDir()
        // This Haiku Go toolchain's fd-relative RemoveAll leaves nonempty
        // directories untouched, including a standalone two-file reproduction.
        // Clean only this test's root with path-based operations before the
        // testing package performs its own cleanup. Never change Bridge/runtime
        // deletion behavior merely to make the integration test pass.
        fixtureRoot := filepath.Dir(directory)
        t.Cleanup(func(){ require.NoError(t, removeMailFixture(fixtureRoot)) })
        configPath := filepath.Join(directory, "client.json")
        certificate := filepath.Join(directory, "bridge.pem")
        var ids []string
        var imapPort, smtpPort int
        for _, phase := range []string{"exercise", "reopen"} {
            withBridge(ctx, t, api.GetHostURL(), ctl, locator, key, func(b *bridge.Bridge, _ *bridge.Mocks) {
                if phase == "exercise" {
                    synced, done := chToType[events.Event, events.SyncFinished](b.GetEvents(events.SyncFinished{}))
                    defer done()
                    for _, name := range []string{username, "mailrecipient", "mailhidden"} {
                        id, err := b.LoginFull(ctx, name, password, nil, nil)
                        require.NoError(t, err)
                        ids = append(ids, id)
                    }
                    for range ids {
                        select { case <-synced: case <-time.After(40*time.Second): t.Fatal("Bridge initial synchronization timed out") }
                    }
                    imapPort, smtpPort = b.GetIMAPPort(), b.GetSMTPPort()
                } else {
                    require.NoError(t, b.SetIMAPPort(ctx, imapPort))
                    require.NoError(t, b.SetSMTPPort(ctx, smtpPort))
                }
                cert, _ := b.GetBridgeTLSCert()
                require.NoError(t, os.WriteFile(certificate, cert, 0600))
                accounts := make([]map[string]string, 0, len(ids))
                for _, id := range ids {
                    info, err := b.GetUserInfo(id)
                    require.NoError(t, err)
                    accounts = append(accounts, map[string]string{"email":info.Addresses[0], "password":string(info.BridgePass)})
                }
                data, err := json.Marshal(map[string]any{"imap":imapPort,"smtp":smtpPort,"certificate":certificate,"accounts":accounts,"directory":directory})
                require.NoError(t, err)
                require.NoError(t, os.WriteFile(configPath, data, 0600))
                runCtx, cancel := context.WithTimeout(ctx, 120*time.Second)
                defer cancel()
                command := exec.CommandContext(runCtx, executable, configPath, phase)
                output, err := command.CombinedOutput()
                t.Logf("Mail %s: %s", phase, output)
                require.NoError(t, err, "native Mail client failed")
                roots := x509.NewCertPool()
                require.True(t, roots.AppendCertsFromPEM(cert))
                for index, account := range accounts {
                    imapClient, err := client.Dial(fmt.Sprintf("127.0.0.1:%d", imapPort))
                    require.NoError(t, err)
                    defer imapClient.Terminate()
                    require.NoError(t, imapClient.StartTLS(&tls.Config{RootCAs:roots,ServerName:"127.0.0.1",MinVersion:tls.VersionTLS12}))
                    require.NoError(t, imapClient.Login(account["email"],account["password"]))
                    sent, err := imapClient.Status("Sent", []imap.StatusItem{imap.StatusMessages})
                    require.NoError(t, err)
                    expectedSent := 1
                    if index == 2 { expectedSent = 0 }
                    require.EqualValues(t, expectedSent, sent.Messages, "Bridge saves one Sent copy for each sending identity")
                    inbox, err := imapClient.Select("INBOX", true)
                    require.NoError(t, err)
                    require.EqualValues(t, 1, inbox.Messages)
                    section := &imap.BodySectionName{Peek:true}
                    messages := make(chan *imap.Message, 4)
                    finished := make(chan error, 1)
                    go func(){ finished <- imapClient.Fetch(&imap.SeqSet{Set:[]imap.Seq{{Start:1,Stop:1}}}, []imap.FetchItem{imap.FetchFlags,section.FetchItem()}, messages) }()
                    for message := range messages {
                        raw, err := io.ReadAll(message.GetBody(section))
                        require.NoError(t, err)
                        parsed, err := mail.ReadMessage(strings.NewReader(string(raw)))
                        require.NoError(t, err)
                        require.Empty(t, parsed.Header.Get("Bcc"), "recipient wire copy must not disclose BCC")
                        sender := 0
                        if index == 0 { sender = 1 }
                        require.Contains(t, parsed.Header.Get("From"), accounts[sender]["email"], "Mail uses the selected account identity")
                        if index == 1 {
                            require.Contains(t, message.Flags, imap.SeenFlag)
                            require.Contains(t, message.Flags, imap.FlaggedFlag)
                        }
                    }
                    require.NoError(t, <-finished)
                    require.NoError(t, imapClient.Logout())
                }
            })
        }
    })
}

func removeMailFixture(path string) error {
    info, err := os.Lstat(path)
    if os.IsNotExist(err) { return nil }
    if err != nil { return err }
    if info.IsDir() {
        entries, err := os.ReadDir(path)
        if err != nil { return err }
        for _, entry := range entries {
            if err := removeMailFixture(filepath.Join(path,entry.Name())); err != nil { return err }
        }
    }
    return os.Remove(path)
}
