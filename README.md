# CS3001 – Computer Networks
## Assignment #1: HTTP Proxy Server
**FAST-NUCES Karachi | Spring 2026**

---

## How to Compile

```bash
make
# OR manually:
gcc -Wall -Wextra -g -o proxy proxy.c
```

## How to Run

```bash
./proxy <port>
# Example:
./proxy 8080
```

## How to Test (without browser)

```bash
# Test 400 Bad Request
echo -e "GARBAGE\r\n\r\n" | curl -s --proxy http://127.0.0.1:8080 http://example.com

# Or use curl directly through proxy:
curl -v --proxy http://127.0.0.1:8080 http://example.com/
curl -v --proxy http://127.0.0.1:8080 http://neverssl.com/
```

---

## How to Configure Your Browser

### Firefox
1. Open Firefox → **Settings** → Search "proxy" → **Settings...**
2. Select **Manual proxy configuration**
3. HTTP Proxy: `127.0.0.1`  Port: `8080`
4. Check **"Also use this proxy for HTTPS"** (optional for HTTP/1.0)
5. Click **OK**

### Chrome
1. Open Chrome → **Settings** → Search "proxy" → **Open your computer's proxy settings**
2. On Linux, or run Chrome directly:
```bash
google-chrome --proxy-server="http://127.0.0.1:8080"
```
3. On Windows: System Settings → Network → Proxy → Manual setup
   - Address: `127.0.0.1`, Port: `8080`

> **Note:** Since this proxy uses HTTP/1.0 only, it works best with plain `http://` sites.
> Try visiting: `http://neverssl.com` or `http://example.com` in your browser.

---

## Features Implemented

| Feature | Status |
|---|---|
| Listens on command-line port (no hardcoded port) | ✅ |
| Accepts concurrent connections via `fork()` | ✅ |
| Max 100 concurrent child processes | ✅ |
| Zombie process cleanup via `SIGCHLD` | ✅ |
| Parses HTTP/1.0 GET requests | ✅ |
| Returns `501 Not Implemented` for non-GET | ✅ |
| Returns `400 Bad Request` for malformed requests | ✅ |
| Absolute URI parsing (host, port, path) | ✅ |
| Default port 80 when none specified | ✅ |
| Forwards request to remote server | ✅ |
| Relays response back to client as-is | ✅ |
| Returns `502 Bad Gateway` if server unreachable | ✅ |
| Returns `503 Service Unavailable` at process limit | ✅ |

---

## Code Structure

```
proxy.c
├── sigchld_handler()   – reaps zombie child processes
├── send_error()        – sends HTTP error responses (400, 501, 502, 503)
├── send_400()          – convenience wrapper
├── send_501()          – convenience wrapper
├── parse_request()     – reads & parses HTTP/1.0 request from client
│     • validates method, version, absolute URI
│     • extracts host, port, path
├── connect_to_server() – opens TCP connection to remote server
├── handle_client()     – child process: parse → connect → relay
└── main()              – setup socket, accept loop, fork()
```

---

## References
- Beej's Guide to Network Programming: http://beej.us/guide/bgnet/
- HTTP Made Really Easy: http://www.jmarshall.com/easy/http/
- RFC 1945 (HTTP/1.0): https://tools.ietf.org/html/rfc1945
- fork() on Wikipedia: https://en.wikipedia.org/wiki/Fork_(system_call)
