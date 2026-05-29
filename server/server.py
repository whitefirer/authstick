"""AuthStick Server — standalone display-code authentication.

Endpoints:
  POST /api/code/create         → generate 4-digit code (body: {site_name})
  GET  /api/code/check?code=X   → poll approval status
  GET  /api/stick/pending?device=MAC    → StickS3 poll
  POST /api/stick/approve               → StickS3 approve (body: {code, device})
  POST /api/stick/deny                  → StickS3 deny (body: {code, device})
  GET  /api/verify?token=X     → validate session token
  GET  /login                  → web login page
  POST /api/logout             → revoke session
"""

import os

from fastapi import FastAPI, Query, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, Response
from pydantic import BaseModel

from auth import CodeStore, DeviceStore, SessionStore

COOKIE_NAME = "stick_sess"
WEB_DIR = os.path.join(os.path.dirname(__file__), "web")


class StickAction(BaseModel):
    code: str
    device: str


class CreateCode(BaseModel):
    site_name: str = ""


# ── global state ──────────────────────────────────────

codes = CodeStore()
sessions = SessionStore()
devices = DeviceStore()


def _set_cookie(response: Response, token: str) -> None:
    response.set_cookie(COOKIE_NAME, token, httponly=True, samesite="strict",
                        max_age=3600)


def _read_cookie(request: Request) -> str:
    return request.cookies.get(COOKIE_NAME, "")


def _err(status: int, msg: str) -> JSONResponse:
    return JSONResponse({"ok": False, "error": msg}, status_code=status)


# ── app ───────────────────────────────────────────────

app = FastAPI(title="AuthStick", version="0.1.0")
app.add_middleware(CORSMiddleware, allow_origins=["*"],
                   allow_methods=["*"], allow_headers=["*"],
                   allow_credentials=True)


# ── login page ────────────────────────────────────────

LOGIN_HTML = """<!DOCTYPE html>
<html lang="zh"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AuthStick 登录</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;
  display:flex;justify-content:center;align-items:center;min-height:100vh}
.panel{background:#16213e;border-radius:12px;padding:2rem;width:100%;max-width:380px;text-align:center}
h1{color:#e94560;margin-bottom:.5rem;font-size:1.5rem}
.sub{color:#889;font-size:.85rem;margin-bottom:1.5rem}
.code-box{background:#0f3460;border:2px dashed #333;border-radius:8px;
  font-size:3rem;letter-spacing:1rem;font-family:monospace;padding:1rem;
  margin:1rem 0;color:#e94560;user-select:all}
.hint{color:#889;font-size:.8rem;margin:.5rem 0}
.status{color:#e94560;font-size:.85rem;margin-top:1rem;min-height:1.5em}
.badge{display:inline-block;background:#0f3460;border-radius:4px;
  padding:2px 8px;font-size:.75rem;color:#aab}
.countdown{color:#e94560;font-weight:bold}
.error{background:#331;color:#f96;padding:8px;border-radius:6px;margin-bottom:1rem}
.hidden{display:none}
</style></head><body>
<div class="panel">
  <h1>AuthStick 登录</h1>
  <p class="sub">打开 AuthStick 设备，验证码将自动显示</p>
  <div id="error" class="error hidden"></div>
  <div class="code-box" id="code">----</div>
  <p class="hint">验证码 <span class="countdown" id="countdown">--</span> 秒后过期</p>
  <p class="status" id="status">等待 Stick 审批...</p>
</div>
<script>
let CODE='',POLL_ID=null,EXPIRES=0;
function showError(m){let e=document.getElementById('error');e.textContent=m;e.classList.remove('hidden')}
function hideError(){document.getElementById('error').classList.add('hidden')}
async function api(method,url,body){
  const opts={method,headers:{'Content-Type':'application/json'},credentials:'include'};
  if(body)opts.body=JSON.stringify(body);
  return (await fetch(url,opts)).json();
}
function startTimer(){
  document.getElementById('countdown').textContent=EXPIRES;
  let t=setInterval(()=>{
    EXPIRES--;document.getElementById('countdown').textContent=EXPIRES;
    if(EXPIRES<=0){clearInterval(t);refresh()}
  },1000);
}
async function refresh(){
  hideError();clearInterval(POLL_ID);
  let r=await api('POST','/api/code/create',{site_name:document.title||''});
  if(!r.ok){showError(r.error||'创建验证码失败');return}
  CODE=r.code;EXPIRES=r.expires_in;
  document.getElementById('code').textContent=CODE;
  document.getElementById('status').textContent='等待 Stick 审批...';
  startTimer();
  POLL_ID=setInterval(poll,2000);
}
async function poll(){
  let r=await fetch('/api/code/check?code='+CODE,{credentials:'include'});
  let d=await r.json();
  if(d.approved){
    document.getElementById('status').textContent='已批准！正在跳转...';
    clearInterval(POLL_ID);
    let params=new URLSearchParams(location.search);
    let redir=params.get('redirect_uri')||'/';
    location.href=redir;
  }
}
refresh();
</script>
</body></html>"""


@app.get("/login")
async def login_page():
    return HTMLResponse(LOGIN_HTML)


# ── code API (web side) ──────────────────────────────

@app.post("/api/code/create")
async def code_create(body: CreateCode = None):
    if body is None:
        body = CreateCode()
    code = codes.create(site_name=body.site_name)
    return {
        "ok": True,
        "code": code,
        "expires_in": 300,
    }


@app.get("/api/code/check")
async def code_check(code: str = Query("")):
    if not code:
        return {"approved": False}
    token = codes.check_approval(code)
    if token:
        resp = JSONResponse({"approved": True, "token": token})
        _set_cookie(resp, token)
        return resp
    return {"approved": False}


# ── stick API (StickS3 side) ─────────────────────────

@app.get("/api/stick/pending")
async def stick_pending(device: str = Query("")):
    return {"codes": codes.list_pending()}


def _check_device(device_mac: str) -> bool:
    """Only registered devices can approve/deny login codes."""
    return devices.is_registered(device_mac)


@app.post("/api/stick/approve")
async def stick_approve(body: StickAction):
    if not _check_device(body.device):
        raise HTTPException(403, detail="设备未注册")
    if codes.approve(body.code, body.device):
        token = sessions.create()
        codes.set_session_token(body.code, token)
        return {"ok": True}
    raise HTTPException(404, detail="验证码不存在或已过期")


@app.post("/api/stick/deny")
async def stick_deny(body: StickAction):
    if not _check_device(body.device):
        raise HTTPException(403, detail="设备未注册")
    if codes.deny(body.code, body.device):
        return {"ok": True}
    raise HTTPException(404, detail="验证码不存在或已过期")


# ── session ──────────────────────────────────────────

@app.get("/api/verify")
async def verify(token: str = Query("")):
    return {"ok": sessions.validate(token)}


@app.post("/api/logout")
async def logout(request: Request):
    sessions.revoke(_read_cookie(request))
    resp = JSONResponse({"ok": True})
    resp.delete_cookie(COOKIE_NAME)
    return resp


# ── device registration (StickS3 side) ──────────────

class DeviceRegister(BaseModel):
    mac: str


@app.post("/api/device/register")
async def device_register(body: DeviceRegister):
    result = devices.register_begin(body.mac)
    if "error" in result:
        raise HTTPException(400, detail=result["error"])
    return {"ok": True, **result}


@app.get("/api/device/status")
async def device_status(mac: str = Query("")):
    return {"registered": devices.check_registration(mac)}


# ── admin ────────────────────────────────────────────

ADMIN_HTML = r"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="UTF-8"><title>AuthStick Admin</title>
<style>
body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:2rem}
h1{color:#e94560}h2{color:#e94560;margin-top:2rem}
table{border-collapse:collapse;width:100%}
th,td{border:1px solid #333;padding:8px;text-align:left}
th{background:#16213e}.btn{background:#e94560;color:#fff;border:none;
padding:6px 14px;cursor:pointer;font-size:14px}
input{background:#0f3460;border:1px solid #333;color:#e0e0e0;padding:8px;margin:6px}
.status-ok{color:#4a4} .status-pending{color:#e94560}
</style></head><body>
<h1>AuthStick 管理后台</h1>

<h2>验证新设备</h2>
<p>在设备屏幕上查看6位验证码，输入下方：</p>
<form onsubmit="verifyDevice(event)">
<input name="code" placeholder="6位数字验证码" maxlength="6" required>
<button class="btn">验证设备</button>
</form>
<div id="verify-msg"></div>

<h2>已注册设备 ({DEVICE_COUNT})</h2>
<table><tr><th>MAC 地址</th><th>名称</th><th>注册时间</th></tr>
{DEVICE_ROWS}
</table>

<script>
async function verifyDevice(e){e.preventDefault();
  const code=e.target.code.value.trim();
  const r=await(await fetch('/api/admin/verify-device',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({code})})).json();
  document.getElementById('verify-msg').textContent=r.ok
    ?'设备 '+r.mac+' 已注册！':(r.error||'验证失败');
  if(r.ok)location.reload();}
</script>
</body></html>"""


class VerifyDevice(BaseModel):
    code: str


@app.get("/admin")
async def admin_page():
    devs = devices.list_registered()
    rows = ""
    for d in devs:
        rows += f"<tr><td>{d['mac']}</td><td>{d.get('name', '')}</td>" \
                f"<td>{d.get('registered_at', '')}</td></tr>"
    if not rows:
        rows = '<tr><td colspan="3">暂无注册设备</td></tr>'
    html = ADMIN_HTML.replace("{DEVICE_COUNT}", str(len(devs)))
    html = html.replace("{DEVICE_ROWS}", rows)
    return HTMLResponse(html)


@app.post("/api/admin/verify-device")
async def admin_verify_device(body: VerifyDevice):
    mac = devices.register_verify(body.code)
    if mac:
        return {"ok": True, "mac": mac}
    raise HTTPException(404, detail="验证码错误或已过期")


# ── main ─────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    import uvicorn

    parser = argparse.ArgumentParser(description="AuthStick Server")
    parser.add_argument("--port", type=int, help="Listen port (env: STICK_PORT)")
    parser.add_argument("--host", default="0.0.0.0", help="Listen host")
    args = parser.parse_args()

    port = args.port or int(os.environ.get("STICK_PORT", 8998))
    uvicorn.run(app, host=args.host, port=port)
