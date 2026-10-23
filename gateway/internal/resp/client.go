package resp

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"strconv"
	"sync/atomic"
	"time"
)

type Options struct {
	Addr           string
	Password       string
	PoolSize       int
	DialTimeout    time.Duration
	CommandTimeout time.Duration
	IdleTimeout    time.Duration
}

type UnavailableError struct {
	Err error
}

func (e *UnavailableError) Error() string {
	return "arca indisponível: " + e.Err.Error()
}

func (e *UnavailableError) Unwrap() error {
	return e.Err
}

type AuthError struct {
	Message string
}

func (e *AuthError) Error() string {
	return "falha de autenticação no arca: " + e.Message
}

type Client struct {
	opts   Options
	idle   chan *conn
	slots  chan struct{}
	closed atomic.Bool
}

type conn struct {
	nc       net.Conn
	r        *bufio.Reader
	w        *bufio.Writer
	lastUsed time.Time
}

func NewClient(opts Options) *Client {
	if opts.PoolSize <= 0 {
		opts.PoolSize = 16
	}
	if opts.DialTimeout <= 0 {
		opts.DialTimeout = 3 * time.Second
	}
	if opts.CommandTimeout <= 0 {
		opts.CommandTimeout = 15 * time.Second
	}
	if opts.IdleTimeout <= 0 {
		opts.IdleTimeout = 2 * time.Minute
	}
	return &Client{
		opts:  opts,
		idle:  make(chan *conn, opts.PoolSize),
		slots: make(chan struct{}, opts.PoolSize),
	}
}

func (c *Client) Close() {
	if c.closed.Swap(true) {
		return
	}
	for {
		select {
		case cn := <-c.idle:
			c.discard(cn)
		default:
			return
		}
	}
}

func (c *Client) dial(ctx context.Context) (*conn, error) {
	d := net.Dialer{Timeout: c.opts.DialTimeout, KeepAlive: 30 * time.Second}
	nc, err := d.DialContext(ctx, "tcp", c.opts.Addr)
	if err != nil {
		return nil, err
	}
	cn := &conn{nc: nc, r: bufio.NewReaderSize(nc, 32<<10), w: bufio.NewWriterSize(nc, 16<<10), lastUsed: time.Now()}
	if c.opts.Password != "" {
		v, err := cn.roundTrip(ctx, c.opts.DialTimeout, []string{"AUTH", c.opts.Password})
		if err != nil {
			nc.Close()
			return nil, err
		}
		if v.Kind == Error {
			nc.Close()
			return nil, &AuthError{Message: v.Str}
		}
	}
	return cn, nil
}

func (c *Client) acquire(ctx context.Context) (*conn, bool, error) {
	if c.closed.Load() {
		return nil, false, errors.New("cliente encerrado")
	}
	for {
		select {
		case cn := <-c.idle:
			if time.Since(cn.lastUsed) > c.opts.IdleTimeout {
				c.discard(cn)
				continue
			}
			return cn, true, nil
		default:
		}
		select {
		case cn := <-c.idle:
			if time.Since(cn.lastUsed) > c.opts.IdleTimeout {
				c.discard(cn)
				continue
			}
			return cn, true, nil
		case c.slots <- struct{}{}:
			cn, err := c.dial(ctx)
			if err != nil {
				<-c.slots
				return nil, false, err
			}
			return cn, false, nil
		case <-ctx.Done():
			return nil, false, ctx.Err()
		}
	}
}

func (c *Client) release(cn *conn) {
	if c.closed.Load() {
		c.discard(cn)
		return
	}
	cn.lastUsed = time.Now()
	select {
	case c.idle <- cn:
	default:
		c.discard(cn)
	}
}

func (c *Client) discard(cn *conn) {
	cn.nc.Close()
	select {
	case <-c.slots:
	default:
	}
}

func (cn *conn) roundTrip(ctx context.Context, timeout time.Duration, args []string) (Value, error) {
	deadline := time.Now().Add(timeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	if err := cn.nc.SetDeadline(deadline); err != nil {
		return Value{}, err
	}
	if err := WriteCommand(cn.w, args); err != nil {
		return Value{}, err
	}
	return ReadValue(cn.r)
}

func (c *Client) Do(ctx context.Context, args ...string) (Value, error) {
	return c.DoTimeout(ctx, c.opts.CommandTimeout, args...)
}

func (c *Client) DoTimeout(ctx context.Context, timeout time.Duration, args ...string) (Value, error) {
	if len(args) == 0 {
		return Value{}, errors.New("comando vazio")
	}
	var lastErr error
	for attempt := 0; attempt < 2; attempt++ {
		cn, reused, err := c.acquire(ctx)
		if err != nil {
			return Value{}, &UnavailableError{Err: err}
		}
		v, err := cn.roundTrip(ctx, timeout, args)
		if err == nil {
			c.release(cn)
			return v, nil
		}
		c.discard(cn)
		lastErr = err
		if !reused || ctx.Err() != nil {
			break
		}
	}
	return Value{}, &UnavailableError{Err: lastErr}
}

func (c *Client) Check(ctx context.Context, args ...string) (Value, error) {
	v, err := c.Do(ctx, args...)
	if err != nil {
		return v, err
	}
	if err := v.Err(); err != nil {
		return v, err
	}
	return v, nil
}

func (c *Client) Text(ctx context.Context, args ...string) (string, error) {
	v, err := c.Check(ctx, args...)
	if err != nil {
		return "", err
	}
	return v.Text(), nil
}

func (c *Client) Int(ctx context.Context, args ...string) (int64, error) {
	v, err := c.Check(ctx, args...)
	if err != nil {
		return 0, err
	}
	switch v.Kind {
	case Integer:
		return v.Int, nil
	case Bulk, SimpleString:
		n, perr := strconv.ParseInt(v.Str, 10, 64)
		if perr != nil {
			return 0, fmt.Errorf("resposta não numérica: %q", v.Str)
		}
		return n, nil
	}
	return 0, fmt.Errorf("resposta inesperada para %s", args[0])
}

func (c *Client) JSON(ctx context.Context, out any, args ...string) error {
	raw, err := c.RawJSON(ctx, args...)
	if err != nil {
		return err
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	return dec.Decode(out)
}

func (c *Client) RawJSON(ctx context.Context, args ...string) (json.RawMessage, error) {
	v, err := c.Check(ctx, args...)
	if err != nil {
		return nil, err
	}
	if v.Kind != Bulk && v.Kind != SimpleString {
		return nil, fmt.Errorf("resposta inesperada para %s", args[0])
	}
	if !json.Valid([]byte(v.Str)) {
		return nil, fmt.Errorf("JSON inválido em %s", args[0])
	}
	return json.RawMessage(v.Str), nil
}

func (c *Client) Ping(ctx context.Context) error {
	v, err := c.DoTimeout(ctx, 2*time.Second, "PING")
	if err != nil {
		return err
	}
	return v.Err()
}
