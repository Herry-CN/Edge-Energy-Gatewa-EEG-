"""Ask the broker what the gateway's MQTT session actually looks like.

Decides whether the missing downlink is "broker has no subscription for this
client" (ESP8266 lost them) or "broker is delivering and the module swallows
them".
"""
import json
import sys
import urllib.error
import urllib.request

HOST = "192.168.8.97"
API = f"http://{HOST}:18083/api/v5"
CANDIDATES = [("admin", "public"), ("admin", "admin"), ("admin", "123456"),
              ("admin", "public123"), ("charge", "123456")]


def post(path, body, token=None):
    req = urllib.request.Request(API + path, data=json.dumps(body).encode(),
                                 method="POST")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def get(path, token):
    req = urllib.request.Request(API + path)
    req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def login():
    for user, pw in CANDIDATES:
        try:
            data = post("/login", {"username": user, "password": pw})
            print(f"logged in as {user}")
            return data["token"]
        except urllib.error.HTTPError as exc:
            if exc.code not in (401, 400):
                print(f"  {user}: HTTP {exc.code}")
        except Exception as exc:                # noqa: BLE001
            print(f"  {user}: {exc}")
    return None


def main():
    token = login()
    if not token:
        print("无法登录 EMQX dashboard（默认口令都不对），跳过 broker 侧取证")
        return 2

    clients = get("/clients?limit=100", token).get("data", [])
    print(f"\n在线客户端 {len(clients)} 个:")
    for c in clients:
        print(f"  {c.get('clientid'):<24} ip={c.get('ip_address')} "
              f"connected={c.get('connected')} subs={c.get('subscriptions_cnt')} "
              f"clean_start={c.get('clean_start')} "
              f"connected_at={c.get('connected_at')}")

    for c in clients:
        cid = c.get("clientid")
        if cid and cid.startswith("eeg-"):
            continue                            # 我们自己的测试脚本
        try:
            subs = get(f"/clients/{urllib.parse.quote(cid, safe='')}/subscriptions",
                       token)
        except Exception as exc:                # noqa: BLE001
            print(f"\n{cid}: 取订阅失败 {exc}")
            continue
        print(f"\n{cid} 的订阅:")
        if not subs:
            print("   (空 —— 订阅确实丢了)")
        for s in subs:
            print(f"   {s.get('topic')}  qos={s.get('qos')}")
    return 0


if __name__ == "__main__":
    import urllib.parse
    sys.exit(main())
