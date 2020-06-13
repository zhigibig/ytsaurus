package httpclient

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"math/rand"
	"net/http"
	"sync"

	"golang.org/x/xerrors"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/log/ctxlog"
	"a.yandex-team.ru/library/go/core/log/nop"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/internal"
	"a.yandex-team.ru/yt/go/yterrors"
)

func decodeYTErrorFromHeaders(h http.Header) (ytErr *yterrors.Error, err error) {
	header := h.Get("X-YT-Error")
	if header == "" {
		return nil, nil
	}

	ytErr = &yterrors.Error{}
	if decodeErr := json.Unmarshal([]byte(header), ytErr); decodeErr != nil {
		err = xerrors.Errorf("yt: malformed 'X-YT-Error' header: %w", decodeErr)
	}

	return
}

type httpClient struct {
	internal.Encoder

	requestLogger   *internal.LoggingInterceptor
	mutationRetrier *internal.MutationRetrier
	readRetrier     *internal.Retrier
	errorWrapper    *internal.ErrorWrapper

	clusterURL yt.ClusterURL
	httpClient *http.Client
	log        log.Structured
	config     *yt.Config
	stop       *internal.StopGroup

	credentials yt.Credentials

	proxiesMu    sync.Mutex
	refreshErr   error
	refreshDone  chan struct{}
	heavyProxies []string
	refreshing   bool
}

func (c *httpClient) doListHeavyProxies(ctx context.Context) ([]string, error) {
	req, err := http.NewRequest("GET", c.clusterURL.URL+"/hosts", nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}

		return nil, err
	}
	defer func() { _ = rsp.Body.Close() }()

	if rsp.StatusCode != 200 {
		return nil, unexpectedStatusCode(rsp)
	}

	var proxies []string
	if err = json.NewDecoder(rsp.Body).Decode(&proxies); err != nil {
		return nil, err
	}

	if len(proxies) == 0 {
		return nil, xerrors.New("proxy list is empty")
	}

	return proxies, nil
}

func (c *httpClient) refreshHeavyProxies(done chan struct{}) {
	defer close(done)
	defer c.stop.Done()

	ctx := c.stop.Context()
	proxies, err := c.doListHeavyProxies(ctx)

	c.proxiesMu.Lock()
	c.refreshing = false
	if err != nil {
		c.refreshErr = err
	} else {
		c.heavyProxies = proxies
	}
	c.proxiesMu.Unlock()
}

func (c *httpClient) listHeavyProxies(ctx context.Context) ([]string, error) {
	c.proxiesMu.Lock()
	if !c.refreshing {
		if !c.stop.TryAdd() {
			c.proxiesMu.Unlock()
			return nil, xerrors.New("client is stopped")
		}

		c.refreshing = true
		c.refreshDone = make(chan struct{})
		go c.refreshHeavyProxies(c.refreshDone)
	}

	refreshDone := c.refreshDone
	proxies := c.heavyProxies
	c.proxiesMu.Unlock()

	if len(proxies) > 0 {
		return proxies, nil
	}

	select {
	case <-refreshDone:
	case <-ctx.Done():
		return nil, fmt.Errorf("error waiting for heavy list: %w", ctx.Err())
	}

	c.proxiesMu.Lock()
	refreshErr := c.refreshErr
	proxies = c.heavyProxies
	c.proxiesMu.Unlock()

	if len(proxies) > 0 {
		return proxies, nil
	}

	return nil, refreshErr
}

func (c *httpClient) pickHeavyProxy(ctx context.Context) (string, error) {
	if c.clusterURL.DisableDiscovery {
		return c.clusterURL.URL, nil
	}

	proxies, err := c.listHeavyProxies(ctx)
	if err != nil {
		return "", err
	}

	return "http://" + proxies[rand.Int()%len(proxies)], nil
}

func (c *httpClient) writeParams(req *http.Request, call *internal.Call) error {
	var params bytes.Buffer

	w := yson.NewWriterFormat(&params, yson.FormatText)
	w.BeginMap()
	call.Params.MarshalHTTP(w)
	w.EndMap()
	if err := w.Finish(); err != nil {
		return err
	}

	h := req.Header
	h.Add("X-YT-Header-Format", "yson")
	if req.Method == http.MethodPost && req.Body == http.NoBody {
		req.Body = ioutil.NopCloser(&params)
		req.ContentLength = int64(params.Len())
		req.GetBody = func() (body io.ReadCloser, e error) {
			return ioutil.NopCloser(&params), nil
		}
	} else {
		h.Add("X-YT-Parameters", params.String())
	}
	h.Add("X-YT-Correlation-ID", call.CallID.String())
	h.Set("User-Agent", "go-yt-client")

	return nil
}

func (c *httpClient) newHTTPRequest(ctx context.Context, call *internal.Call, body io.Reader) (req *http.Request, err error) {
	var url string
	if call.ProxyURL != "" {
		url = call.ProxyURL
	} else if call.Params.HTTPVerb().IsHeavy() {
		url, err = c.pickHeavyProxy(ctx)
		if err != nil {
			return nil, err
		}
	} else {
		url = c.clusterURL.URL
	}

	if body == nil {
		body = bytes.NewBuffer(call.YSONValue)
	}

	verb := call.Params.HTTPVerb()
	req, err = http.NewRequest(verb.HTTPMethod(), url+"/api/v4/"+verb.String(), body)
	if err != nil {
		return
	}

	if err = c.writeParams(req, call); err != nil {
		return
	}

	if body != nil {
		req.Header.Add("X-YT-Input-Format", "yson")
	}
	req.Header.Add("X-YT-Header-Format", "<format=text>yson")
	req.Header.Add("X-YT-Output-Format", "yson")

	if requestCredentials := yt.ContextCredentials(ctx); requestCredentials != nil {
		requestCredentials.Set(req)
	} else if c.credentials != nil {
		c.credentials.Set(req)
	}

	c.logRequest(ctx, req)
	return
}

func (c *httpClient) logRequest(ctx context.Context, req *http.Request) {
	ctxlog.Debug(ctx, c.log.Logger(), "sending HTTP request",
		log.String("proxy", req.URL.Hostname()))
}

func (c *httpClient) logResponse(ctx context.Context, rsp *http.Response) {
	ctxlog.Debug(ctx, c.log.Logger(), "received HTTP response",
		log.String("yt_proxy", rsp.Header.Get("X-YT-Proxy")),
		log.String("yt_request_id", rsp.Header.Get("X-YT-Request-Id")))
}

// unexpectedStatusCode is last effort attempt to get useful error message from a failed request.
func unexpectedStatusCode(rsp *http.Response) error {
	d := json.NewDecoder(rsp.Body)
	d.UseNumber()

	var ytErr yterrors.Error
	if err := d.Decode(&ytErr); err == nil {
		return &ytErr
	}

	return xerrors.Errorf("unexpected status code %d", rsp.StatusCode)
}

func (c *httpClient) readResult(rsp *http.Response) (res *internal.CallResult, err error) {
	defer func() { _ = rsp.Body.Close() }()

	res = &internal.CallResult{}

	var ytErr *yterrors.Error
	ytErr, err = decodeYTErrorFromHeaders(rsp.Header)
	if err != nil {
		return
	}
	if ytErr != nil {
		return nil, ytErr
	}

	if rsp.StatusCode/100 != 2 {
		return nil, unexpectedStatusCode(rsp)
	}

	res.YSONValue, err = ioutil.ReadAll(rsp.Body)
	return
}

func (c *httpClient) do(ctx context.Context, call *internal.Call) (res *internal.CallResult, err error) {
	var req *http.Request
	req, err = c.newHTTPRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}
	}

	if err == nil {
		c.logResponse(ctx, rsp)
		res, err = c.readResult(rsp)
	}

	return
}

func (c *httpClient) doWrite(ctx context.Context, call *internal.Call) (w io.WriteCloser, err error) {
	pr, pw := io.Pipe()
	errChan := make(chan error, 1)

	req, err := c.newHTTPRequest(ctx, call, ioutil.NopCloser(pr))
	if err != nil {
		return nil, err
	}

	go func() {
		defer close(errChan)

		rsp, err := c.httpClient.Do(req.WithContext(ctx))
		closeErr := func(err error) {
			errChan <- err
			_ = pr.CloseWithError(err)
		}

		if err != nil {
			closeErr(err)
			return
		}

		c.logResponse(ctx, rsp)
		defer func() { _ = rsp.Body.Close() }()

		if rsp.StatusCode/100 == 2 {
			return
		}

		callErr, err := decodeYTErrorFromHeaders(rsp.Header)
		if err != nil {
			closeErr(err)
			return
		}

		if callErr != nil {
			closeErr(callErr)
			return
		}

		closeErr(unexpectedStatusCode(rsp))
	}()

	w = &httpWriter{p: pw, errChan: errChan}
	return
}

func (c *httpClient) doWriteRow(ctx context.Context, call *internal.Call) (w yt.TableWriter, err error) {
	var ww io.WriteCloser

	ctx, cancelFunc := context.WithCancel(ctx)
	ww, err = c.InvokeWrite(ctx, call)
	if err != nil {
		cancelFunc()
		return
	}

	w = newTableWriter(ww, cancelFunc)
	return
}

func (c *httpClient) doReadRow(ctx context.Context, call *internal.Call) (r yt.TableReader, err error) {
	if call.OnRspParams != nil {
		panic("call.OnRspParams is already set")
	}

	var rspParams *tableReaderRspParams
	call.OnRspParams = func(ys []byte) (err error) {
		rspParams, err = decodeRspParams(ys)
		return
	}

	var rr io.ReadCloser
	rr, err = c.InvokeRead(ctx, call)
	if err != nil {
		return
	}

	tr := newTableReader(rr)

	if rspParams != nil {
		if err := tr.setRspParams(rspParams); err != nil {
			return nil, xerrors.Errorf("invalid rsp params: %w", err)
		}
	}

	r = tr
	return
}

func (c *httpClient) doRead(ctx context.Context, call *internal.Call) (r io.ReadCloser, err error) {
	var req *http.Request
	req, err = c.newHTTPRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		return nil, err
	}

	c.logResponse(ctx, rsp)
	if rsp.StatusCode != 200 {
		defer func() { _ = rsp.Body.Close() }()

		var callErr *yterrors.Error
		callErr, err = decodeYTErrorFromHeaders(rsp.Header)
		if err == nil && callErr != nil {
			err = callErr
		} else {
			err = unexpectedStatusCode(rsp)
		}
	}

	if err == nil && call.OnRspParams != nil {
		if p := rsp.Header.Get("X-YT-Response-Parameters"); p != "" {
			err = call.OnRspParams([]byte(p))
		}
	}

	if err == nil {

		r = &httpReader{
			body: rsp.Body,
			rsp:  rsp,
		}
	}

	return
}

func (c *httpClient) BeginTx(ctx context.Context, options *yt.StartTxOptions) (yt.Tx, error) {
	return internal.NewTx(ctx, c.Encoder, c.stop, options)
}

func (c *httpClient) Stop() {
	c.stop.Stop()
}

func NewHTTPClient(c *yt.Config) (yt.Client, error) {
	var client httpClient

	if c.Logger != nil {
		client.log = c.Logger
	} else {
		client.log = &nop.Logger{}
	}

	client.config = c
	client.clusterURL = yt.NormalizeProxyURL(c.Proxy)
	client.httpClient = http.DefaultClient
	client.stop = internal.NewStopGroup()

	client.Encoder.Invoke = client.do
	client.Encoder.InvokeRead = client.doRead
	client.Encoder.InvokeReadRow = client.doReadRow
	client.Encoder.InvokeWrite = client.doWrite
	client.Encoder.InvokeWriteRow = client.doWriteRow

	client.mutationRetrier = &internal.MutationRetrier{Log: client.log}
	client.readRetrier = &internal.Retrier{Log: client.log}
	client.requestLogger = &internal.LoggingInterceptor{Structured: client.log}
	client.errorWrapper = &internal.ErrorWrapper{}

	client.Encoder.Invoke = client.Encoder.Invoke.
		Wrap(client.requestLogger.Intercept).
		Wrap(client.mutationRetrier.Intercept).
		Wrap(client.readRetrier.Intercept).
		Wrap(client.errorWrapper.Intercept)

	client.Encoder.InvokeRead = client.Encoder.InvokeRead.
		Wrap(client.requestLogger.Read).
		Wrap(client.errorWrapper.Read)

	client.Encoder.InvokeWrite = client.Encoder.InvokeWrite.
		Wrap(client.requestLogger.Write).
		Wrap(client.errorWrapper.Write)

	if c.Token != "" {
		client.credentials = &yt.TokenCredentials{Token: c.Token}
	}

	return &client, nil
}
