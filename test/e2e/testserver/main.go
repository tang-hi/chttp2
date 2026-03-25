package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	mrand "math/rand"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

type connKey struct{}

type server struct {
	httpSrv *http.Server
	mu      sync.Mutex
	conns   map[net.Conn]struct{}
}

func (s *server) trackConn(c net.Conn) {
	s.mu.Lock()
	s.conns[c] = struct{}{}
	s.mu.Unlock()
}

func (s *server) untrackConn(c net.Conn) {
	s.mu.Lock()
	delete(s.conns, c)
	s.mu.Unlock()
}

func getConn(r *http.Request) net.Conn {
	c, _ := r.Context().Value(connKey{}).(net.Conn)
	return c
}

func getRawTCPConn(c net.Conn) *net.TCPConn {
	switch v := c.(type) {
	case *net.TCPConn:
		return v
	case *tls.Conn:
		if tc, ok := v.NetConn().(*net.TCPConn); ok {
			return tc
		}
	}
	return nil
}

func main() {
	useTLS := flag.Bool("tls", false, "Enable TLS")
	flag.Parse()

	srv := &server{conns: make(map[net.Conn]struct{})}
	mux := http.NewServeMux()
	srv.registerHandlers(mux)

	h2s := &http2.Server{}
	var handler http.Handler
	if *useTLS {
		handler = mux
	} else {
		handler = h2c.NewHandler(mux, h2s)
	}

	srv.httpSrv = &http.Server{
		Handler: handler,
		ConnContext: func(ctx context.Context, c net.Conn) context.Context {
			srv.trackConn(c)
			return context.WithValue(ctx, connKey{}, c)
		},
		ConnState: func(c net.Conn, state http.ConnState) {
			if state == http.StateClosed {
				srv.untrackConn(c)
			}
		},
	}
	if *useTLS {
		http2.ConfigureServer(srv.httpSrv, h2s)
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatalf("listen: %v", err)
	}

	_, port, _ := net.SplitHostPort(ln.Addr().String())

	if *useTLS {
		tlsCert, certPath := generateSelfSignedCert()
		defer os.Remove(certPath)
		tlsCfg := &tls.Config{
			Certificates: []tls.Certificate{tlsCert},
			NextProtos:   []string{"h2"},
		}
		ln = tls.NewListener(ln, tlsCfg)
		fmt.Printf("LISTENING :%s\n", port)
		fmt.Printf("CERT %s\n", certPath)
	} else {
		fmt.Printf("LISTENING :%s\n", port)
	}
	os.Stdout.Sync()

	log.Fatal(srv.httpSrv.Serve(ln))
}

func (s *server) registerHandlers(mux *http.ServeMux) {
	mux.HandleFunc("/echo", echoHandler)
	mux.HandleFunc("/echo-body", echoBodyHandler)
	mux.HandleFunc("/big", bigHandler)
	mux.HandleFunc("/upload", uploadHandler)
	mux.HandleFunc("/delay", delayHandler)
	mux.HandleFunc("/status/", statusHandler)
	mux.HandleFunc("/headers/large", largeHeadersHandler)
	mux.HandleFunc("/trailers", trailersHandler)
	mux.HandleFunc("/head", headHandler)
	mux.HandleFunc("/drip", dripHandler)
	mux.HandleFunc("/goaway", s.goawayHandler)
	mux.HandleFunc("/reset", resetHandler)
	mux.HandleFunc("/close", s.closeHandler)
	mux.HandleFunc("/ping", pingHandler)
}

// GET /echo - echo request headers as JSON
func echoHandler(w http.ResponseWriter, r *http.Request) {
	hdrs := make(map[string]string)
	for k, v := range r.Header {
		hdrs[strings.ToLower(k)] = v[0]
	}
	hdrs[":method"] = r.Method
	hdrs[":path"] = r.URL.RequestURI()
	hdrs[":authority"] = r.Host

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(hdrs)
}

// POST /echo-body - echo request body verbatim
func echoBodyHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/octet-stream")
	io.Copy(w, r.Body)
}

// GET /big?size=N - deterministic pseudo-random body
func bigHandler(w http.ResponseWriter, r *http.Request) {
	size, _ := strconv.Atoi(r.URL.Query().Get("size"))
	if size <= 0 {
		size = 1024
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	rng := mrand.New(mrand.NewSource(42))
	buf := make([]byte, 8192)
	remaining := size
	for remaining > 0 {
		n := len(buf)
		if n > remaining {
			n = remaining
		}
		for i := 0; i < n; i++ {
			buf[i] = byte(rng.Intn(256))
		}
		w.Write(buf[:n])
		remaining -= n
	}
}

// POST /upload - return received body size as JSON
func uploadHandler(w http.ResponseWriter, r *http.Request) {
	n, _ := io.Copy(io.Discard, r.Body)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"size":%d}`, n)
}

// GET /delay?ms=N - delay then respond
func delayHandler(w http.ResponseWriter, r *http.Request) {
	ms, _ := strconv.Atoi(r.URL.Query().Get("ms"))
	if ms > 0 {
		time.Sleep(time.Duration(ms) * time.Millisecond)
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "ok")
}

// GET /status/CODE - respond with given status code
func statusHandler(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(r.URL.Path, "/")
	code := 200
	if len(parts) >= 3 {
		code, _ = strconv.Atoi(parts[2])
	}
	if code < 100 || code > 599 {
		code = 200
	}
	w.WriteHeader(code)
	if code != 204 && code != 304 {
		fmt.Fprintf(w, "%d", code)
	}
}

// GET /headers/large - ~32KB of response headers (triggers CONTINUATION)
func largeHeadersHandler(w http.ResponseWriter, r *http.Request) {
	for i := 0; i < 100; i++ {
		w.Header().Set(fmt.Sprintf("x-large-header-%03d", i), strings.Repeat("x", 300))
	}
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(200)
	fmt.Fprintf(w, "ok")
}

// GET /trailers - response with trailer headers
func trailersHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Trailer", "X-Checksum, X-Timestamp")
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(200)
	io.WriteString(w, "response with trailers")
	w.Header().Set("X-Checksum", "abc123")
	w.Header().Set("X-Timestamp", "2025-01-01T00:00:00Z")
}

// HEAD /head - HEAD with Content-Length, no body
func headHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Content-Length", "1024")
	w.WriteHeader(200)
}

// GET /drip?size=N&chunk=C&delay=D - streaming response
func dripHandler(w http.ResponseWriter, r *http.Request) {
	size, _ := strconv.Atoi(r.URL.Query().Get("size"))
	chunk, _ := strconv.Atoi(r.URL.Query().Get("chunk"))
	delayMs, _ := strconv.Atoi(r.URL.Query().Get("delay"))
	if size <= 0 {
		size = 1024
	}
	if chunk <= 0 {
		chunk = 128
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.WriteHeader(200)
	flusher, _ := w.(http.Flusher)
	buf := make([]byte, chunk)
	for i := range buf {
		buf[i] = 'x'
	}
	sent := 0
	for sent < size {
		n := chunk
		if sent+n > size {
			n = size - sent
		}
		w.Write(buf[:n])
		sent += n
		if flusher != nil {
			flusher.Flush()
		}
		if delayMs > 0 && sent < size {
			time.Sleep(time.Duration(delayMs) * time.Millisecond)
		}
	}
}

// GET /goaway - send GOAWAY then close connection
func (s *server) goawayHandler(w http.ResponseWriter, r *http.Request) {
	conn := getConn(r)
	if conn == nil {
		http.Error(w, "no conn", 500)
		return
	}
	tc := getRawTCPConn(conn)
	if tc == nil {
		http.Error(w, "no tcp conn", 500)
		return
	}
	// Write a raw GOAWAY frame directly to TCP.
	// Frame: length=8, type=GOAWAY(0x07), flags=0, stream=0
	// Payload: last_stream_id=0, error_code=NO_ERROR(0)
	goawayFrame := []byte{
		0, 0, 8,    // length: 8
		0x07,        // type: GOAWAY
		0x00,        // flags
		0, 0, 0, 0, // stream id: 0
		0, 0, 0, 0, // last stream id: 0
		0, 0, 0, 0, // error code: NO_ERROR
	}
	tc.Write(goawayFrame)
	time.Sleep(50 * time.Millisecond)
	tc.Close()
}

// GET /reset - send RST_STREAM
func resetHandler(w http.ResponseWriter, r *http.Request) {
	// Panic with ErrAbortHandler causes Go's h2 server to send RST_STREAM.
	panic(http.ErrAbortHandler)
}

// GET /close - abruptly close TCP (no GOAWAY)
func (s *server) closeHandler(w http.ResponseWriter, r *http.Request) {
	conn := getConn(r)
	if conn == nil {
		http.Error(w, "no conn", 500)
		return
	}
	tc := getRawTCPConn(conn)
	if tc == nil {
		http.Error(w, "no tcp conn", 500)
		return
	}
	// Set linger to 0 for immediate TCP RST (no FIN handshake).
	tc.SetLinger(0)
	tc.Close()
}

// GET /ping - simple health check
func pingHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "pong")
}

func generateSelfSignedCert() (tls.Certificate, string) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		log.Fatalf("generate key: %v", err)
	}

	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
		DNSNames:     []string{"localhost"},
	}

	certDER, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		log.Fatalf("create cert: %v", err)
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certDER})
	keyDER, _ := x509.MarshalECPrivateKey(key)
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER})

	tlsCert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		log.Fatalf("x509 key pair: %v", err)
	}

	// Write cert to temp file so the C++ client can load it.
	f, err := os.CreateTemp("", "chttp2_e2e_cert_*.pem")
	if err != nil {
		log.Fatalf("create temp cert: %v", err)
	}
	f.Write(certPEM)
	f.Close()

	return tlsCert, f.Name()
}
