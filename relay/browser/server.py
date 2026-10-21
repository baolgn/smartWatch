"""serve validated XWR snapshots to a foreground Bluetooth browser."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hmac
import http.client
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import socket
from socketserver import ThreadingMixIn
import ssl
import sys
import threading
import time
from urllib.parse import urlsplit

# share source validation and XWR encoding with the standalone parser.
ROOT = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT.parent))
import data_sources

REFRESH_SECONDS = 900
FUTURE_SECONDS = 300
MAX_CLIENTS = 8
TOKEN_PATTERN = re.compile(r"[A-Za-z0-9_-]{32,128}\Z", re.ASCII)
ASSETS = {"/": ("index.html", "text/html; charset=utf-8"),
          "/index.html": ("index.html", "text/html; charset=utf-8"),
          "/relay_client.js": ("relay_client.js", "text/javascript; charset=utf-8"),
          "/relay_protocol.js": ("relay_protocol.js", "text/javascript; charset=utf-8"),
          "/style.css": ("style.css", "text/css; charset=utf-8")}


# keep published snapshots immutable across HTTP and refresh threads.
@dataclass(frozen=True)
class Packet:
    topic: str
    sequence: int
    body: str


# publish all configured topics together after validation succeeds.
@dataclass(frozen=True)
class Snapshot:
    fetched_utc: int
    fetched_monotonic: float
    packets: tuple


# accept one to eight private source definitions, including local files.
def sources(options):
    data_sources.require(not options.topic or options.url is not None or options.file is not None,
                      "Use --topic only with --url or --file")
    base = Path.cwd()
    # resolve configured file paths relative to the configuration file.
    if options.config:
        config = data_sources.read_json_file(options.config)
        data_sources.require(type(config) is dict and set(config) == {"sources"},
                          "Config must contain only sources")
        selected = config["sources"]
        base = Path(options.config).resolve().parent
    # a single generic source requires its explicit non-weather topic.
    elif options.url is not None or options.file is not None:
        kind = "url" if options.url is not None else "file"
        selected = [{"topic": options.topic, kind: getattr(options, kind)}]
    # use the existing fixed-location weather adapter by default.
    else:
        selected = [{"topic": "weather", "adapter": "weather"}]
    data_sources.require(type(selected) is list and 1 <= len(selected) <= 8,
                      "Select one to eight sources")
    topics, result = set(), []
    # validate the complete configuration before fetching any source.
    for source in selected:
        data_sources.require(type(source) is dict, "Source must be an object")
        topic = source.get("topic")
        data_sources.require(type(topic) is str and data_sources.TOPIC_PATTERN.fullmatch(topic)
                          and topic not in topics, "Source topics must be valid and unique")
        topics.add(topic)
        # reserve weather for the lossless forecast adapter.
        if source == {"topic": "weather", "adapter": "weather"}:
            result.append(dict(source))
            continue
        data_sources.require(topic != "weather", "Weather requires the weather adapter")
        data_sources.require(set(source) in ({"topic", "url"}, {"topic", "file"}),
                          "Generic source requires topic and either url or file")
        kind = "url" if "url" in source else "file"
        data_sources.require(type(source[kind]) is str and bool(source[kind]),
                          "Source address must be a nonempty string")
        # HTTPS inputs share redirect and response limits.
        if kind == "url":
            data_sources.validate_url(source[kind])
            result.append(dict(source))
        # local paths never become HTTP routes or response fields.
        else:
            result.append({"topic": topic, "file": base / source["file"]})
    return tuple(result)


# fetch every source and encode bounded XW1 text frames.
def fetch_snapshot(selected):
    packets = []
    # fetch sources serially to bound sockets and publish an atomic batch.
    for source in selected:
        if source.get("adapter") == "weather":
            payload = data_sources.pack_weather(data_sources.fetch_weather())
        elif "url" in source:
            payload = data_sources.fetch_json(source["url"])
        else:
            payload = data_sources.read_json_file(source["file"])
        body = data_sources.envelope(source["topic"], payload)
        decoded = data_sources.decode_envelope(body)
        packets.append(Packet(decoded["topic"], decoded["seq"], body.decode("ascii")))
    now = int(time.time())
    data_sources.require(all(-FUTURE_SECONDS <= now - item.sequence <= REFRESH_SECONDS
                          for item in packets), "Fetched batch is stale or future-dated")
    return Snapshot(now, time.monotonic(), tuple(packets))


# retain a valid snapshot during transient failures without extending its age.
class SnapshotStore:
    def __init__(self, selected):
        self.selected = selected
        self.lock = threading.Lock()
        self.snapshot = None
        self.stop = threading.Event()

    # replace the shared reference only after every packet passes validation.
    def refresh(self):
        snapshot = fetch_snapshot(self.selected)
        with self.lock:
            self.snapshot = snapshot
        return snapshot

    # use elapsed time as well as UTC to reject clock rollback and stale data.
    def current(self):
        with self.lock:
            snapshot = self.snapshot
        now, elapsed = int(time.time()), time.monotonic()
        if snapshot is None or not 0 <= elapsed - snapshot.fetched_monotonic <= REFRESH_SECONDS:
            return None
        if not -FUTURE_SECONDS <= now - snapshot.fetched_utc <= REFRESH_SECONDS:
            return None
        if not all(-FUTURE_SECONDS <= now - item.sequence <= REFRESH_SECONDS
                   for item in snapshot.packets):
            return None
        return {"version": 1, "server_utc": now, "fetched_utc": snapshot.fetched_utc,
                "packets": [{"topic": item.topic, "sequence": item.sequence, "body": item.body}
                            for item in snapshot.packets]}

    # refresh immediately, then keep a monotonic fifteen-minute cadence.
    def run(self):
        while not self.stop.is_set():
            started = time.monotonic()
            try:
                snapshot = self.refresh()
                print("snapshot ready: packets=%d" % len(snapshot.packets), flush=True)
            except (data_sources.SourceError, OSError, ValueError, OverflowError):
                print("snapshot refresh failed; previous data retains its original expiry",
                      file=sys.stderr, flush=True)
            self.stop.wait(max(1, REFRESH_SECONDS - (time.monotonic() - started)))


# obtain a bearer secret without displaying it or accepting command-line secrets.
def access_token():
    token = os.environ.get("WATCH_RELAY_TOKEN")
    if token is None:
        directory = ROOT / ".local"
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / "access_token.txt"
        data_sources.require(not path.is_symlink(), "Token file must not be a symbolic link")
        try:
            descriptor = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            with path.open("r", encoding="ascii") as source:
                token = source.read(130).strip()
        else:
            token = secrets.token_urlsafe(32)
            with os.fdopen(descriptor, "w", encoding="ascii") as destination:
                destination.write(token + "\n")
    data_sources.require(type(token) is str and TOKEN_PATTERN.fullmatch(token),
                      "Access token must contain 32-128 URL-safe characters")
    return token.encode("ascii")


# cap combined header bytes independently of Python's per-line limits.
class HeaderReader:
    def __init__(self, stream):
        self.stream, self.total = stream, 0

    # abort oversized or accumulated headers with HTTP 431.
    def readline(self, limit=-1):
        data = self.stream.readline(8193 if limit < 0 else min(limit, 8193))
        self.total += len(data)
        if len(data) > 8192 or self.total > 32768:
            raise http.client.LineTooLong("header budget")
        return data


# bound concurrent clients and suppress exception details containing request data.
class RelayServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    request_queue_size = MAX_CLIENTS

    def __init__(self, address, token, store, context=None):
        self.token, self.store, self.context = token, store, context
        self.slots = threading.BoundedSemaphore(MAX_CLIENTS)
        super().__init__(address, RelayHandler)

    # reject overload before creating another worker.
    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    # stop slow clients even when individual reads remain below socket timeout.
    def expire_request(self, request):
        try:
            request.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.close_request(request)

    # perform TLS and HTTP work within bounded connection deadlines.
    def process_request_thread(self, request, address):
        timer = None
        try:
            request.settimeout(5)
            if self.context is not None:
                request = self.context.wrap_socket(request, server_side=True)
            timer = threading.Timer(10, self.expire_request, args=(request,))
            timer.daemon = True
            timer.start()
            super().process_request_thread(request, address)
        except (OSError, ssl.SSLError):
            self.shutdown_request(request)
        finally:
            if timer is not None:
                timer.cancel()
            self.slots.release()

    # avoid logging request bodies, URLs, source paths or secrets.
    def handle_error(self, request, client_address):
        pass


# expose only fixed assets and an authenticated snapshot endpoint.
class RelayHandler(BaseHTTPRequestHandler):
    server_version = "WatchRelay"
    sys_version = ""

    # keep reads short and process only one request per connection.
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    # enforce aggregate header limits before endpoint handling.
    def parse_request(self):
        if len(self.raw_requestline) > 2048:
            self.requestline, self.request_version, self.command = "", "HTTP/1.0", None
            self.close_connection = True
            self.send_error(414)
            return False
        original = self.rfile
        self.rfile = HeaderReader(original)
        try:
            return super().parse_request()
        finally:
            self.rfile = original

    # log no access headers or URL fragments forwarded by malformed clients.
    def log_message(self, format, *args):
        pass

    # use bounded constant errors instead of echoing request text.
    def send_error(self, code, message=None, explain=None):
        self.respond(code, b'{"error":"request_rejected"}', "application/json")

    # disable caching, framing and external resources while allowing Bluetooth.
    def respond(self, status, body, content_type="application/json"):
        self.close_connection = True
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Permissions-Policy", "bluetooth=(self)")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; "
                         "style-src 'self'; connect-src 'self'; img-src 'self' data:; "
                         "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    # require exactly one bounded bearer header and compare in constant time.
    def authorized(self):
        headers = self.headers.get_all("Authorization", [])
        if len(headers) != 1 or not headers[0].startswith("Bearer ") or len(headers[0]) > 160:
            return False
        try:
            return hmac.compare_digest(headers[0][7:].encode("ascii"), self.server.token)
        except UnicodeError:
            return False

    # separate API authentication from public fixed assets without CORS.
    def do_GET(self):
        try:
            parsed = urlsplit(self.path)
        except ValueError:
            self.send_error(400)
            return
        if parsed.scheme or parsed.netloc or parsed.query or parsed.fragment:
            self.send_error(400)
            return
        if parsed.path == "/api/snapshot":
            if not self.authorized():
                self.respond(401, b'{"error":"unauthorized"}')
                return
            result = self.server.store.current()
            if result is None:
                self.respond(503, b'{"error":"snapshot_unavailable"}')
            else:
                self.respond(200, json.dumps(result, separators=(",", ":")).encode("ascii"))
            return
        asset = ASSETS.get(parsed.path)
        if asset is None:
            self.send_error(404)
            return
        try:
            path = ROOT / asset[0]
            if path.is_symlink():
                self.send_error(404)
                return
            with path.open("rb") as source:
                body = source.read(256 * 1024 + 1)
            if len(body) > 256 * 1024:
                self.send_error(500)
                return
        except OSError:
            self.send_error(404)
            return
        self.respond(200, body, asset[1])


# collect explicit network exposure and source settings without credentials.
def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--weather", action="store_true", help="use ZIP 10018 weather (default)")
    group.add_argument("--url", help="generic HTTPS JSON source")
    group.add_argument("--file", help="generic local JSON source")
    group.add_argument("--config", help="private JSON configuration with one to eight sources")
    parser.add_argument("--topic", help="generic source topic; weather is reserved")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default: loopback)")
    parser.add_argument("--port", type=int, default=8765, help="listen port (default: 8765)")
    parser.add_argument("--cert", help="TLS certificate PEM file")
    parser.add_argument("--key", help="TLS private key PEM file")
    parser.add_argument("--check", action="store_true", help="fetch and validate once; print only counts")
    return parser.parse_args(argv)


# start a foreground server only when explicitly invoked without --check.
def main(argv=None):
    options = arguments(argv)
    store = None
    try:
        data_sources.require(1 <= options.port <= 65535, "Port must be between 1 and 65535")
        data_sources.require(bool(options.cert) == bool(options.key), "TLS requires both --cert and --key")
        selected = sources(options)
        if options.check:
            snapshot = fetch_snapshot(selected)
            print("validated: packets=%d total_body_bytes=%d" %
                  (len(snapshot.packets), sum(len(item.body) for item in snapshot.packets)))
            return 0
        context = None
        if options.cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(options.cert, options.key)
        store = SnapshotStore(selected)
        with RelayServer((options.host, options.port), access_token(), store, context) as server:
            worker = threading.Thread(target=store.run, daemon=True)
            worker.start()
            print("browser relay ready on port %d; refresh every 900 seconds" % options.port, flush=True)
            server.serve_forever(poll_interval=0.5)
        return 0
    except KeyboardInterrupt:
        return 130
    except (data_sources.SourceError, OSError, ValueError, OverflowError):
        print("relay failed: check source configuration, token, TLS files and bind settings",
              file=sys.stderr)
        return 1
    finally:
        if store is not None:
            store.stop.set()


# importing helpers never starts a listener or refresh worker.
if __name__ == "__main__":
    raise SystemExit(main())
