# -*- coding: utf-8 -*-

import base64
import ipaddress
import urllib.parse
import urllib.request

from flask import Flask, request, Response, render_template_string
import yaml


app = Flask(__name__)


# ============================================================
# 基础工具
# ============================================================

USER_AGENT = (
    "Mozilla/5.0 "
    "(Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 "
    "(KHTML, like Gecko) "
    "Chrome/154.0.0.0 Safari/537.36"
)


def b64encode(text: str) -> str:
    """
    v2rayN 常用的标准 Base64。
    """
    return base64.b64encode(
        text.encode("utf-8")
    ).decode("ascii")


def quote_name(name) -> str:
    return urllib.parse.quote(
        str(name or ""),
        safe=""
    )


def quote_value(value) -> str:
    return urllib.parse.quote(
        str(value),
        safe=""
    )


def format_host(host) -> str:
    """
    IPv6 地址需要 [ ]。
    """
    host = str(host)

    try:
        ip = ipaddress.ip_address(host)

        if ip.version == 6:
            return f"[{host}]"

    except ValueError:
        pass

    return host


def format_host_port(host, port) -> str:
    return f"{format_host(host)}:{int(port)}"


def truthy(value) -> bool:
    return str(value).lower() not in (
        "",
        "false",
        "0",
        "no",
        "off",
        "none",
        "null",
    )


# ============================================================
# SS
# ============================================================

def convert_ss(node):
    name = node.get("name", "SS")

    server = node.get("server")
    port = node.get("port")
    cipher = node.get("cipher") or node.get("method")
    password = node.get("password")

    if not all((server, port, cipher, password)):
        raise ValueError(
            "SS 缺少 server / port / cipher / password"
        )

    # SIP002
    userinfo = b64encode(
        f"{cipher}:{password}"
    ).rstrip("=")

    uri = (
        f"ss://{userinfo}@"
        f"{format_host_port(server, port)}"
    )

    plugin = node.get("plugin")

    if plugin:
        uri += (
            "?plugin="
            + quote_value(plugin)
        )

    return uri + "#" + quote_name(name)


# ============================================================
# VLESS
# ============================================================

def convert_vless(node):
    name = node.get("name", "VLESS")

    server = node.get("server")
    port = node.get("port")
    uuid = node.get("uuid")

    if not all((server, port, uuid)):
        raise ValueError(
            "VLESS 缺少 server / port / uuid"
        )

    network = node.get("network", "tcp")

    params = {
        "type": network,
        "encryption": "none",
    }

    # flow
    flow = node.get("flow")

    if flow:
        params["flow"] = flow

    # --------------------------------------------------------
    # TLS / Reality
    # --------------------------------------------------------

    tls = truthy(node.get("tls"))

    reality = (
        node.get("reality-opts")
        or node.get("reality_opts")
    )

    if reality:
        params["security"] = "reality"

    elif tls:
        params["security"] = "tls"

    else:
        params["security"] = "none"

    # --------------------------------------------------------
    # SNI
    # --------------------------------------------------------

    sni = (
        node.get("servername")
        or node.get("serverName")
        or node.get("sni")
    )

    if sni:
        params["sni"] = sni

    # --------------------------------------------------------
    # Fingerprint
    # --------------------------------------------------------

    fingerprint = (
        node.get("client-fingerprint")
        or node.get("client_fingerprint")
    )

    if fingerprint:
        params["fp"] = fingerprint

    # --------------------------------------------------------
    # Reality
    # --------------------------------------------------------

    if reality:

        public_key = (
            reality.get("public-key")
            or reality.get("public_key")
        )

        short_id = (
            reality.get("short-id")
            or reality.get("short_id")
        )

        if public_key:
            params["pbk"] = public_key

        if short_id:
            params["sid"] = short_id

    # --------------------------------------------------------
    # ALPN
    # --------------------------------------------------------

    alpn = node.get("alpn")

    if alpn:

        if isinstance(alpn, list):
            params["alpn"] = ",".join(
                str(x) for x in alpn
            )

        else:
            params["alpn"] = str(alpn)

    query = urllib.parse.urlencode(
        params,
        quote_via=urllib.parse.quote
    )

    return (
        "vless://"
        + urllib.parse.quote(
            str(uuid),
            safe=""
        )
        + "@"
        + format_host_port(server, port)
        + "?"
        + query
        + "#"
        + quote_name(name)
    )


# ============================================================
# Trojan
# ============================================================

def convert_trojan(node):
    name = node.get("name", "Trojan")

    server = node.get("server")
    port = node.get("port")
    password = node.get("password")

    if not all((server, port, password)):
        raise ValueError(
            "Trojan 缺少 server / port / password"
        )

    params = {
        "security": "tls"
    }

    sni = (
        node.get("sni")
        or node.get("servername")
        or node.get("serverName")
    )

    if sni:
        params["sni"] = sni

    # Clash:
    # skip-cert-verify: true
    #
    # v2ray URI:
    # allowInsecure=1
    if truthy(node.get("skip-cert-verify")):
        params["allowInsecure"] = "1"

    network = node.get("network")

    if network and network != "tcp":
        params["type"] = network

    query = urllib.parse.urlencode(
        params,
        quote_via=urllib.parse.quote
    )

    return (
        "trojan://"
        + urllib.parse.quote(
            str(password),
            safe=""
        )
        + "@"
        + format_host_port(server, port)
        + "?"
        + query
        + "#"
        + quote_name(name)
    )


# ============================================================
# AnyTLS
# ============================================================

def convert_anytls(node):
    """
    AnyTLS 常见 URI：

    anytls://password@server:port/?sni=example.com

    注意：
    Clash/Mihomo 中的 ech-opts 不在这里伪造转换。
    """

    name = node.get("name", "AnyTLS")

    server = node.get("server")
    port = node.get("port")
    password = node.get("password")

    if not all((server, port, password)):
        raise ValueError(
            "AnyTLS 缺少 server / port / password"
        )

    params = {}

    sni = (
        node.get("sni")
        or node.get("servername")
    )

    if sni:
        params["sni"] = sni

    if truthy(node.get("skip-cert-verify")):
        params["insecure"] = "1"

    # 某些 Mihomo 配置可能包含这些 AnyTLS 参数
    for key in (
        "idle-session-check-interval",
        "idle-session-timeout",
        "min-idle-session",
    ):
        if node.get(key) is not None:
            params[key] = str(node[key])

    if params:
        query = "?" + urllib.parse.urlencode(
            params,
            quote_via=urllib.parse.quote
        )
    else:
        query = ""

    return (
        "anytls://"
        + urllib.parse.quote(
            str(password),
            safe=""
        )
        + "@"
        + format_host_port(server, port)
        + "/"
        + query
        + "#"
        + quote_name(name)
    )


# ============================================================
# 协议转换表
# ============================================================

CONVERTERS = {
    "ss": convert_ss,
    "vless": convert_vless,
    "trojan": convert_trojan,
    "anytls": convert_anytls,
}


# ============================================================
# Clash → URI
# ============================================================

def convert_clash(text):

    data = yaml.safe_load(text)

    if not isinstance(data, dict):
        raise ValueError(
            "远端内容不是有效的 YAML"
        )

    proxies = data.get("proxies")

    if not isinstance(proxies, list):
        raise ValueError(
            "YAML 中没有 proxies"
        )

    links = []
    unsupported = []
    failed = []

    for node in proxies:

        if not isinstance(node, dict):
            continue

        protocol = str(
            node.get("type", "")
        ).lower()

        name = node.get(
            "name",
            "(unnamed)"
        )

        converter = CONVERTERS.get(
            protocol
        )

        if converter is None:

            unsupported.append(
                f"{name} [{protocol or 'unknown'}]"
            )

            continue

        try:

            uri = converter(node)

            links.append(uri)

        except Exception as e:

            failed.append(
                f"{name} [{protocol}]: {e}"
            )

    return links, unsupported, failed


# ============================================================
# 下载远程 Clash 配置
# ============================================================

def download_subscription(url):

    parsed = urllib.parse.urlparse(url)

    if parsed.scheme not in (
        "http",
        "https"
    ):
        raise ValueError(
            "订阅 URL 只允许 http / https"
        )

    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "*/*",
        }
    )

    with urllib.request.urlopen(
        request,
        timeout=30
    ) as response:

        raw = response.read()

        content_type = response.headers.get(
            "Content-Type",
            ""
        )

    # UTF-8 优先
    text = raw.decode(
        "utf-8-sig",
        errors="replace"
    )

    return text


# ============================================================
# 动态 /sub
# ============================================================

@app.route("/sub")
def subscription():

    source_url = request.args.get(
        "url",
        ""
    ).strip()

    if not source_url:

        return Response(
            "missing url parameter\n",
            status=400,
            content_type="text/plain; charset=utf-8"
        )

    try:

        # 1. 下载最新 Clash
        clash_text = download_subscription(
            source_url
        )

        # 2. 实时转换
        links, unsupported, failed = (
            convert_clash(clash_text)
        )

        if not links:

            detail = (
                "没有转换出任何节点。\n"
                f"unsupported={len(unsupported)}\n"
                f"failed={len(failed)}\n"
            )

            if unsupported:
                detail += (
                    "\nUnsupported:\n"
                    + "\n".join(
                        unsupported[:20]
                    )
                )

            if failed:
                detail += (
                    "\nFailed:\n"
                    + "\n".join(
                        failed[:20]
                    )
                )

            return Response(
                detail,
                status=502,
                content_type=(
                    "text/plain; charset=utf-8"
                )
            )

        # 3. v2rayN Base64
        body = "\n".join(links)

        encoded = b64encode(body)

        # 4. 返回给客户端
        return Response(
            encoded,
            status=200,
            content_type=(
                "text/plain; charset=utf-8"
            ),
            headers={
                "Cache-Control": "no-store, "
                                  "no-cache, "
                                  "must-revalidate",
                "Pragma": "no-cache",
            }
        )

    except Exception as e:

        return Response(
            "subscription conversion failed:\n"
            + str(e)
            + "\n",
            status=502,
            content_type=(
                "text/plain; charset=utf-8"
            )
        )


# ============================================================
# 网页首页
# ============================================================

HTML = r"""
<!DOCTYPE html>

<html lang="zh-CN">

<head>

<meta charset="UTF-8">

<meta
    name="viewport"
    content="width=device-width,initial-scale=1"
>

<title>Clash → v2rayN</title>

<style>

body {
    font-family:
        -apple-system,
        BlinkMacSystemFont,
        "Segoe UI",
        sans-serif;

    max-width: 900px;

    margin: 50px auto;

    padding: 0 20px;

    background: #f5f5f5;
}

.card {
    background: white;

    padding: 30px;

    border-radius: 14px;

    box-shadow:
        0 4px 20px
        rgba(0,0,0,.08);
}

input {
    width: 100%;

    box-sizing: border-box;

    padding: 12px;

    border: 1px solid #ccc;

    border-radius: 8px;

    margin: 8px 0;
}

button {
    padding: 11px 18px;

    border: 0;

    border-radius: 8px;

    cursor: pointer;
}

pre {
    white-space: pre-wrap;

    word-break: break-all;

    background: #f0f0f0;

    padding: 15px;

    border-radius: 8px;
}

code {
    background: #eee;

    padding: 2px 5px;

    border-radius: 4px;
}

</style>

</head>

<body>

<div class="card">

<h1>Clash → v2rayN</h1>

<p>
无数据库、动态 URL、实时转换。
</p>

<h3>生成订阅链接</h3>

<form onsubmit="generate(event)">

<input
    id="url"
    placeholder="https://example.com/subscription"
    required
>

<button type="submit">
生成 v2rayN 订阅
</button>

</form>

<h3>v2rayN URL</h3>

<pre id="result">等待输入...</pre>

<h3>接口</h3>

<ul>

<li>
<code>/sub?url=...</code>
实时转换 Base64
</li>

<li>
<code>/health</code>
服务状态
</li>

</ul>

</div>

<script>

function generate(event) {

    event.preventDefault();

    const source =
        document.getElementById("url").value;

    const endpoint =
        location.origin
        + "/sub?url="
        + encodeURIComponent(source);

    document.getElementById("result")
        .textContent = endpoint;
}

</script>

</body>

</html>
"""


@app.route("/")
def index():
    return render_template_string(HTML)


# ============================================================
# Health Check
# ============================================================

@app.route("/health")
def health():

    return Response(
        "OK\n",
        status=200,
        content_type="text/plain; charset=utf-8"
    )


# ============================================================
# 启动
# ============================================================

if __name__ == "__main__":

    print()
    print("=" * 60)
    print("Clash → v2rayN Dynamic Subscription Converter")
    print("=" * 60)
    print()
    print("Web:")
    print("  http://127.0.0.1:8667/")
    print()
    print("Subscription:")
    print("  /sub?url=<encoded-clash-url>")
    print()
    print("Health:")
    print("  /health")
    print()
    print("=" * 60)
    print()

    app.run(
        host="0.0.0.0",
        port=8667,
        threaded=True
    )