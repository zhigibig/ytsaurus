package rpcclient

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/url"

	"github.com/golang/protobuf/proto"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/xerrors"
	"a.yandex-team.ru/yt/go/bus"
	"a.yandex-team.ru/yt/go/yt/internal"
	"a.yandex-team.ru/yt/go/yterrors"
)

const ProtocolVersionMajor = 1

func (c *client) listRPCProxies() ([]string, error) {
	if !c.stop.TryAdd() {
		return nil, xerrors.New("client is stopped")
	}
	defer c.stop.Done()

	v := url.Values{"type": {"rpc"}}
	if c.conf.ProxyRole != "" {
		v.Add("role", c.conf.ProxyRole)
	}

	var resolveURL url.URL
	resolveURL.Scheme = c.schema()
	resolveURL.Host = c.httpClusterURL.Address
	resolveURL.Path = "api/v4/discover_proxies"
	resolveURL.RawQuery = v.Encode()

	req, err := http.NewRequest("GET", resolveURL.String(), nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(c.stop.Context()))
	if err != nil {
		return nil, err
	}
	defer func() { _ = rsp.Body.Close() }()

	if rsp.StatusCode != http.StatusOK {
		return nil, unexpectedStatusCode(rsp)
	}

	var proxies struct {
		Proxies []string `json:"proxies"`
	}
	if err = json.NewDecoder(rsp.Body).Decode(&proxies); err != nil {
		return nil, err
	}

	if len(proxies.Proxies) == 0 {
		return nil, xerrors.New("rpc proxy list is empty")
	}

	return proxies.Proxies, nil
}

func (c *client) pickRPCProxy(ctx context.Context) (string, error) {
	if c.rpcClusterURL.DisableDiscovery {
		return c.rpcClusterURL.Address, nil
	}

	proxy, err := c.proxySet.PickRandom(ctx)
	if err != nil {
		return "", err
	}

	return proxy, nil
}

type ProxyBouncer struct {
	Log log.Structured

	ProxySet *internal.ProxySet
	ConnPool *connPool
}

func (b *ProxyBouncer) banProxy(call *Call, err error) {
	if !shouldBanProxy(err) {
		return
	}

	b.Log.Debug("banning rpc proxy", log.String("fqdn", call.SelectedProxy))
	b.ProxySet.BanProxy(call.SelectedProxy)
	b.ConnPool.Discard(call.SelectedProxy)
}

func (b *ProxyBouncer) Intercept(ctx context.Context, call *Call, next CallInvoker, rsp proto.Message, opts ...bus.SendOption) error {
	err := next(ctx, call, rsp, opts...)
	b.banProxy(call, err)
	return err
}

func shouldBanProxy(err error) bool {
	if err == nil {
		return false
	}

	var opErr *net.OpError
	if errors.As(err, &opErr) {
		return true
	}

	return isProxyBannedError(err)
}

func isProxyBannedError(err error) bool {
	var ytErr *yterrors.Error
	if errors.As(err, &ytErr) && ytErr.Message == "Proxy is banned" {
		return true
	}

	return false
}
