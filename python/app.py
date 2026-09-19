#!/usr/bin/env python3
"""
mxy-harness 前端：Flask + WebSocket + SSE + HTTP

浏览器 <--WebSocket/HTTP--> Flask <--Unix Socket--> C++ mxy-harnessd <--HTTPS--> 任意 LLM API

[Fix #2]  daemon_stream 加了 read timeout（默认 600s），避免 /api/chat 挂死。
[Fix 小-7] WSSession.active 加了锁，避免 register / pop 竞态。
"""
import json
import os
import socket
import threading
import uuid

from flask import Flask, render_template, request, jsonify, Response, stream_with_context
from flask_sock import Sock

SOCK_PATH = os.environ.get("MXY_HARNESS_SOCK", "/tmp/mxy-harness.sock")

app = Flask(__name__)
app.config["SOCK_SERVER_OPTIONS"] = {"ping_interval": 25}
sock = Sock(app)


# ------------------------------------------------------------------ #
# 与 C++ 守护进程通信
# ------------------------------------------------------------------ #
def _new_id() -> str:
    return uuid.uuid4().hex[:16]


def daemon_call(cmd: str, params: dict = None, timeout: float = 30.0):
    """快命令（非流式）。"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCK_PATH)
        rid = _new_id()
        payload = json.dumps({"id": rid, "cmd": cmd, "params": params or {}}) + "\n"
        s.sendall(payload.encode())
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                t = ev.get("type")
                if t in ("result", "done"):
                    return ev.get("data")
                if t == "error":
                    raise RuntimeError(ev.get("message", "daemon error"))
    finally:
        s.close()
    raise RuntimeError("daemon closed connection without reply")


# [Fix #2] 增加 read timeout
def daemon_stream(cmd: str, params: dict, read_timeout: float = 600.0):
    """流式命令，yield 每个事件（dict）。"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(read_timeout)
    try:
        s.connect(SOCK_PATH)
    except Exception:
        s.close()
        raise
    rid = _new_id()
    s.sendall((json.dumps({"id": rid, "cmd": cmd, "params": params or {}}) + "\n").encode())
    buf = b""
    try:
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                yield {"type": "error", "message": f"daemon read timeout ({read_timeout}s)"}
                return
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                yield ev
                if ev.get("type") in ("done", "error", "result"):
                    return
    finally:
        s.close()


# ------------------------------------------------------------------ #
# WebSocket
# ------------------------------------------------------------------ #
class WSSession:
    def __init__(self, ws):
        self.ws = ws
        self.lock = threading.Lock()
        self.active = {}          # request_id -> daemon socket
        self.alive = True

    def send(self, obj):
        with self.lock:
            if not self.alive:
                return False
            try:
                self.ws.send(json.dumps(obj, ensure_ascii=False))
                return True
            except Exception:
                self.alive = False
                return False

    # [Fix 小-7] 加锁
    def register(self, rid, ds):
        with self.lock:
            self.active[rid] = ds

    def unregister(self, rid):
        with self.lock:
            self.active.pop(rid, None)

    def get_active(self, rid):
        with self.lock:
            return self.active.get(rid)


def _ws_worker(sess: WSSession, rid: str, cmd: str, params: dict):
    try:
        ds = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ds.connect(SOCK_PATH)
    except Exception as e:
        sess.send({"id": rid, "type": "error", "message": f"daemon connect failed: {e}"})
        return

    sess.register(rid, ds)
    try:
        ds.sendall((json.dumps({"id": rid, "cmd": cmd, "params": params or {}}) + "\n").encode())
        buf = b""
        while True:
            chunk = ds.recv(65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                sess.send(ev)
                if ev.get("type") in ("done", "error", "result"):
                    return
    except Exception as e:
        sess.send({"id": rid, "type": "error", "message": str(e)})
    finally:
        sess.unregister(rid)
        try:
            ds.close()
        except Exception:
            pass


@sock.route("/ws")
def ws_endpoint(ws):
    sess = WSSession(ws)
    while sess.alive:
        try:
            raw = ws.receive()
        except Exception:
            break
        if raw is None:
            break
        try:
            req = json.loads(raw)
        except Exception:
            continue

        rid = str(req.get("id") or _new_id())
        cmd = req.get("cmd") or ""
        params = req.get("params") or {}

        if cmd == "cancel":
            # 取消目标请求：关闭它那条到 daemon 的连接 → daemon 侧读到 EOF → cancel 置位 → curl 被杀
            target = str(params.get("target") or "")
            ds = sess.get_active(target)
            if ds:
                try:
                    ds.close()
                except Exception:
                    pass
            continue

        threading.Thread(
            target=_ws_worker, args=(sess, rid, cmd, params), daemon=True
        ).start()

    sess.alive = False


# ------------------------------------------------------------------ #
# HTTP API
# ------------------------------------------------------------------ #
def _api(fn):
    try:
        return jsonify({"ok": True, "data": fn()})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/ping")
def api_ping():
    return _api(lambda: daemon_call("ping"))


@app.route("/api/models", methods=["GET"])
def api_models():
    return _api(lambda: daemon_call("list_models"))


@app.route("/api/models", methods=["POST"])
def api_save_model():
    body = request.get_json(force=True)
    return _api(lambda: daemon_call("save_model", body))


@app.route("/api/models/<mid>", methods=["DELETE"])
def api_delete_model(mid):
    return _api(lambda: daemon_call("delete_model", {"id": mid}))


@app.route("/api/conversations", methods=["GET"])
def api_convs():
    return _api(lambda: daemon_call("list_conversations"))


@app.route("/api/conversations", methods=["POST"])
def api_new_conv():
    body = request.get_json(force=True, silent=True) or {}
    return _api(lambda: daemon_call("new_conversation", body))


@app.route("/api/conversations/<cid>", methods=["GET"])
def api_get_conv(cid):
    return _api(lambda: daemon_call("get_conversation", {"conv_id": cid}))


@app.route("/api/conversations/<cid>", methods=["DELETE"])
def api_del_conv(cid):
    return _api(lambda: daemon_call("delete_conversation", {"conv_id": cid}))


@app.route("/api/conversations/<cid>/rename", methods=["POST"])
def api_rename_conv(cid):
    body = request.get_json(force=True, silent=True) or {}
    return _api(lambda: daemon_call("rename_conversation",
                                    {"conv_id": cid, "title": body.get("title", "")}))


@app.route("/api/chat", methods=["POST"])
def api_chat():
    body = request.get_json(force=True)
    content = ""
    final = None
    try:
        for ev in daemon_stream("chat", body):
            t = ev.get("type")
            if t == "delta":
                content += ev.get("text", "")
            elif t == "done":
                final = ev.get("data")
            elif t == "error":
                return jsonify({"ok": False, "error": ev.get("message")}), 500
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500
    return jsonify({"ok": True, "data": final or {"content": content}})


@app.route("/api/chat/stream", methods=["POST"])
def api_chat_stream():
    body = request.get_json(force=True)

    def gen():
        try:
            for ev in daemon_stream("chat", body):
                yield "data: " + json.dumps(ev, ensure_ascii=False) + "\n\n"
        except Exception as e:
            yield "data: " + json.dumps({"type": "error", "message": str(e)}) + "\n\n"

    return Response(
        stream_with_context(gen()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8080"))
    host = os.environ.get("HOST", "127.0.0.1")
    print(f"[mxy-harness-ui] http://{host}:{port}   (daemon socket: {SOCK_PATH})")
    app.run(host=host, port=port, threaded=True, debug=False)