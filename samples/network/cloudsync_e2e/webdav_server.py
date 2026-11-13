#!/usr/bin/env python3
"""A WebDAV server for cloud sync tests, standard library only.

Serves a directory on 127.0.0.1 with the methods RetroArch's WebDAV
cloud sync driver sends (OPTIONS, GET, PUT, MKCOL, DELETE, COPY, MOVE),
Basic auth, a request log and faults a test can switch on while it runs:

  down              refuse every request (the connection is closed)
  status            {(method, path_suffix): code}, or {"*": code}
  fail_at           (n, code): answer request n (1-based) with code
  delay             {path_suffix: seconds} before answering
  truncate          {path_suffix: True}: a GET sends half its body, closes
  die_after         n: requests after the nth are dropped unanswered

Used in-process by run.py; run on its own it prints the port:

  webdav_server.py ROOT [USER PASSWORD]
"""

import base64
import http.server
import os
import shutil
import socket
import sys
import threading
import time
import urllib.parse


class DavState:
    def __init__(self, root, user=None, password=None):
        self.root     = os.path.abspath(root)
        self.user     = user
        self.password = password
        self.lock     = threading.Lock()
        self.fslock   = threading.Lock()
        self.log      = []
        self.count    = 0
        self.reset_faults()

    def reset_faults(self):
        self.down      = False
        self.status    = {}
        self.fail_at   = None
        self.delay     = {}
        self.truncate  = {}
        self.die_after = None

    def requests(self, method=None, prefix=""):
        with self.lock:
            return [(m, p, s) for (m, p, s) in self.log
                    if (method is None or m == method) and p.startswith(prefix)]

    def clear_log(self):
        with self.lock:
            self.log   = []
            self.count = 0


def _match(table, path):
    for suffix, value in table.items():
        if path.endswith(suffix):
            return value
    return None


class DavHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version   = "cloudsync-e2e-dav"

    def log_message(self, fmt, *args):
        pass

    @property
    def st(self):
        return self.server.dav

    def _rel(self, url):
        path = urllib.parse.urlsplit(url).path
        return urllib.parse.unquote(path).lstrip("/")

    def _fs(self, rel):
        full = os.path.normpath(os.path.join(self.st.root, rel))
        if full != self.st.root and not full.startswith(self.st.root + os.sep):
            return None
        return full

    def _body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    return data
                data += self.rfile.read(size)
                self.rfile.readline()
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def _send(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _drop(self):
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def _authorized(self):
        if self.st.user is None:
            return True
        want = base64.b64encode(("%s:%s" % (self.st.user, self.st.password))
                                .encode()).decode()
        return self.headers.get("Authorization", "") == "Basic " + want

    def _handle(self):
        rel = self._rel(self.path)
        st  = self.st
        body = self._body()
        with st.lock:
            st.count += 1
            n = st.count
            down = st.down or (st.die_after is not None and n > st.die_after)
            fail = st.fail_at[1] if st.fail_at and st.fail_at[0] == n else None
            forced = st.status.get("*")
            for key, value in st.status.items():
                if isinstance(key, tuple) and key[0] == self.command \
                        and rel.endswith(key[1]):
                    forced = value
            delay = _match(st.delay, rel)
            trunc = _match(st.truncate, rel)
        if down:
            self._record(rel, 0)
            self._drop()
            return
        if delay:
            time.sleep(delay)
        if not self._authorized():
            self._record(rel, 401)
            self._send(401, b"", {"WWW-Authenticate": 'Basic realm="e2e"'})
            return
        code = fail or forced
        if code:
            self._record(rel, code)
            self._send(code)
            return
        with st.fslock:
            code = getattr(self, "dav_" + self.command)(rel, body, trunc)
        self._record(rel, code)

    def _record(self, rel, code):
        with self.st.lock:
            self.st.log.append((self.command, rel, code))

    def dav_OPTIONS(self, rel, body, trunc):
        self._send(200, b"", {"DAV": "1, 2",
                              "Allow": "OPTIONS, GET, PUT, DELETE, MKCOL, COPY, MOVE"})
        return 200

    def dav_GET(self, rel, body, trunc):
        fs = self._fs(rel)
        if not fs or not os.path.isfile(fs):
            self._send(404)
            return 404
        with open(fs, "rb") as f:
            data = f.read()
        if trunc:
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data[:len(data) // 2])
            self.wfile.flush()
            self._drop()
            return 200
        self._send(200, data)
        return 200

    def dav_PUT(self, rel, body, trunc):
        fs = self._fs(rel)
        if not fs or not os.path.isdir(os.path.dirname(fs)):
            self._send(409)
            return 409
        existed = os.path.exists(fs)
        tmp = fs + ".dav-upload"
        with open(tmp, "wb") as f:
            f.write(body)
        os.replace(tmp, fs)
        code = 204 if existed else 201
        self._send(code)
        return code

    def dav_MKCOL(self, rel, body, trunc):
        fs = self._fs(rel.rstrip("/"))
        if not fs:
            self._send(403)
            return 403
        if os.path.exists(fs):
            self._send(405)
            return 405
        if not os.path.isdir(os.path.dirname(fs)):
            self._send(409)
            return 409
        os.mkdir(fs)
        self._send(201)
        return 201

    def dav_DELETE(self, rel, body, trunc):
        fs = self._fs(rel)
        if not fs or not os.path.exists(fs):
            self._send(404)
            return 404
        if os.path.isdir(fs):
            shutil.rmtree(fs)
        else:
            os.remove(fs)
        self._send(204)
        return 204

    def _copy_or_move(self, rel, move):
        src  = self._fs(rel)
        dest = self._fs(self._rel(self.headers.get("Destination", "")))
        if not src or not dest or not os.path.exists(src):
            self._send(404)
            return 404
        if not os.path.isdir(os.path.dirname(dest)):
            self._send(409)
            return 409
        existed = os.path.exists(dest)
        if existed and self.headers.get("Overwrite", "T") == "F":
            self._send(412)
            return 412
        if move:
            os.replace(src, dest)
        else:
            shutil.copyfile(src, dest)
        code = 204 if existed else 201
        self._send(code)
        return code

    def dav_COPY(self, rel, body, trunc):
        return self._copy_or_move(rel, False)

    def dav_MOVE(self, rel, body, trunc):
        return self._copy_or_move(rel, True)

    def do_any(self):
        try:
            self._handle()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    do_OPTIONS = do_GET = do_PUT = do_MKCOL = do_DELETE = do_COPY = \
        do_MOVE = do_HEAD = do_PROPFIND = do_any

    def dav_HEAD(self, rel, body, trunc):
        return self.dav_GET(rel, body, trunc)

    def dav_PROPFIND(self, rel, body, trunc):
        self._send(405)
        return 405


class DavServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, state):
        super().__init__(("127.0.0.1", 0), DavHandler)
        self.dav = state

    @property
    def port(self):
        return self.server_address[1]

    def start(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.shutdown()
        self.server_close()


def main():
    if len(sys.argv) not in (2, 4):
        print(__doc__)
        return 2
    root = sys.argv[1]
    os.makedirs(root, exist_ok=True)
    state = DavState(root, *(sys.argv[2:4] or [None, None]))
    srv = DavServer(state)
    print("port %d" % srv.port, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
