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

from auth import CodeStore, DeviceStore, SessionStore, DISPLAY_CODE_TTL

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
  font-size:2rem;letter-spacing:.6rem;font-family:monospace;padding:.8rem .4rem;
  margin:1rem 0;color:#e94560;user-select:all;width:100%}
.hint{color:#889;font-size:.8rem;margin:.5rem 0}
.status{color:#e94560;font-size:.85rem;margin-top:1rem;min-height:1.5em}
.badge{display:inline-block;background:#0f3460;border-radius:4px;
  padding:2px 8px;font-size:.75rem;color:#aab}
.countdown{color:#e94560;font-weight:bold}
.error{background:#331;color:#f96;padding:8px;border-radius:6px;margin-bottom:1rem}
.hidden{display:none}
</style></head><body>
<div class="panel">
  <h1>输入验证码</h1>
  <p class="sub">查看 AuthStick 设备上显示的 6 位验证码，填入下方</p>
  <div id="error" class="error hidden"></div>
  <input type="text" id="codeInput" class="code-box" placeholder="输入6位验证码" maxlength="6" autocomplete="off" style="text-align:center">
  <button id="submitBtn" onclick="submitCode()" style="background:#e94560;color:#fff;border:none;padding:10px 30px;border-radius:6px;font-size:1rem;cursor:pointer;margin-top:1rem">提交验证</button>
  <p class="status" id="status"></p>
</div>
<script>
function showError(m){let e=document.getElementById('error');e.textContent=m;e.classList.remove('hidden')}
async function submitCode(){
  let input=document.getElementById('codeInput').value.trim();
  if(!input){showError('请输入验证码');return}
  let r=await fetch('/api/code/verify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:input}),credentials:'include'}).then(r=>r.json());
  if(r.ok){
    document.getElementById('status').textContent='验证成功！正在跳转...';
    let params=new URLSearchParams(location.search);
    location.href=params.get('redirect_uri')||'/';
  }else{
    showError(r.error||'验证失败');
  }
}
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


class CodeVerify(BaseModel):
    code: str

@app.post("/api/code/verify")
async def code_verify(body: CodeVerify):
    if not body.code or not body.code.strip():
        return {"ok": False, "error": "请输入验证码"}
    token = codes.approve(body.code.strip(), "web")
    if not token:
        return {"ok": False, "error": "验证码无效或已过期"}
    session = sessions.create()
    resp = JSONResponse({"ok": True})
    _set_cookie(resp, session)
    return resp


# ── stick API (StickS3 side) ─────────────────────────

@app.get("/api/stick/pending")
async def stick_pending(device: str = Query("")):
    name = devices.get_name(device) if device else ""
    banned = devices.is_banned(device) if device else False
    return {"codes": codes.list_pending(), "device_name": name, "banned": banned}

class GenerateCode(BaseModel):
    token: str
    mac: str = ""

class RotateToken(BaseModel):
    token: str
    mac: str = ""

@app.post("/api/stick/rotate-token")
async def stick_rotate_token(body: RotateToken):
    mac = devices.lookup_by_token(body.token)
    if not mac:
        return JSONResponse({"error": "invalid token"}, status_code=403)
    if body.mac and body.mac.upper().replace(":", "") != mac.replace(":", ""):
        return JSONResponse({"error": "token/MAC mismatch"}, status_code=403)
    new_token = devices.rotate_token(mac)
    if not new_token:
        return JSONResponse({"error": "rotate failed"}, status_code=500)
    return {"ok": True, "device_token": new_token}

@app.post("/api/stick/generate")
async def stick_generate(body: GenerateCode):
    mac = devices.lookup_by_token(body.token)
    if not mac:
        return JSONResponse({"error": "invalid token"}, status_code=403)
    if body.mac and body.mac.upper().replace(":", "") != mac.replace(":", ""):
        return JSONResponse({"error": "token/MAC mismatch"}, status_code=403)
    name = devices.get_name(mac)
    banned = devices.is_banned(mac)
    if banned:
        return {"device_name": name, "banned": True}
    code = codes.create(site_name="", device=mac)
    return {"code": code, "expires_in": DISPLAY_CODE_TTL, "device_name": name, "banned": False}


def _check_device(device_mac: str) -> bool:
    """Only registered, non-banned devices can operate."""
    return devices.is_registered(device_mac) and not devices.is_banned(device_mac)


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


class BindToken(BaseModel):
    mac: str
    code: str
    device_token: str

@app.post("/api/device/bind-token")
async def device_bind_token(body: BindToken):
    if devices.bind_token(body.mac, body.code, body.device_token):
        return {"ok": True}
    raise HTTPException(400, detail="invalid code or MAC mismatch")


@app.get("/api/device/status")
async def device_status(mac: str = Query("")):
    name = devices.get_name(mac) if devices.is_registered(mac) else ""
    return {
        "registered": devices.check_registration(mac),
        "banned": devices.is_banned(mac),
        "name": name,
    }


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
<input name="code" placeholder="6位数字注册码" maxlength="6" required>
<input name="devname" placeholder="设备名称（选填）">
<button class="btn">验证设备</button>
</form>
<div id="verify-msg"></div>

<h2>已注册设备 ({DEVICE_COUNT})</h2>
<table><tr><th>MAC</th><th>名称</th><th>状态</th><th>操作</th></tr>
{DEVICE_ROWS}
</table>

<script>
async function api(url,body){return (await fetch(url,{method:'POST',
  headers:{'Content-Type':'application/json'},
  body:JSON.stringify(body||{})})).json();}
async function verifyDevice(e){e.preventDefault();
  const code=e.target.code.value.trim();
  const name=e.target.devname.value.trim();
  let r=await api('/api/admin/verify-device',{code,name});
  let msg=document.getElementById('verify-msg');
  if(r.ok){msg.style.color='';msg.textContent='设备 '+r.mac+' 已注册！'}
  else{msg.style.color='#e94560';msg.textContent=r.detail||'注册码无效或已过期';}
  if(r.ok)location.reload();}
async function renameDev(mac){
  const n=prompt('新名称:',''); if(!n)return;
  await api('/api/admin/rename',{mac,name:n}); location.reload();}
async function banDev(mac){await api('/api/admin/ban',{mac}); location.reload();}
async function unbanDev(mac){await api('/api/admin/unban',{mac}); location.reload();}
async function resetToken(mac){
  if(!confirm('重置令牌后设备需重新注册，确认？'))return;
  await api('/api/admin/reset-token',{mac}); location.reload();}
async function removeDev(mac){
  if(!confirm('确认移除设备 '+mac+'?'))return;
  await api('/api/admin/remove',{mac}); location.reload();}
</script>
</body></html>"""


class VerifyDevice(BaseModel):
    code: str


@app.get("/admin")
async def admin_page():
    devs = devices.list_registered()
    rows = ""
    for d in devs:
        banned = d.get('banned', False)
        has_token = bool(d.get('token', ''))
        if banned: status = '<span class="banned">已封禁</span>'
        elif not has_token: status = '<span class="status-pending">待注册</span>'
        else: status = '<span class="status-ok">正常</span>'
        mac = d['mac']
        name = d.get('name', mac)
        rows += f"<tr><td>{mac}</td><td onclick=\"renameDev('{mac}')\" style=\"cursor:pointer\" title=\"点击改名\">{name}</td>" \
                f"<td>{status}</td>" \
                f"<td>" \
                f"<button class=\"btn-sm\" onclick=\"renameDev('{mac}')\">改名</button> " \
                f"<button class=\"btn-sm\" onclick=\"{'unbanDev' if banned else 'banDev'}('{mac}')\">{'解封' if banned else '封禁'}</button> " \
                f"<button class=\"btn-sm\" onclick=\"resetToken('{mac}')\">重置令牌</button> " \
                f"<button class=\"btn-sm\" onclick=\"removeDev('{mac}')\">移除</button>" \
                f"</td></tr>"
    if not rows:
        rows = '<tr><td colspan="3">暂无注册设备</td></tr>'
    html = ADMIN_HTML.replace("{DEVICE_COUNT}", str(len(devs)))
    html = html.replace("{DEVICE_ROWS}", rows)
    return HTMLResponse(html)


class VerifyDevice(BaseModel):
    code: str
    name: str = ""

@app.post("/api/admin/verify-device")
async def admin_verify_device(body: VerifyDevice):
    mac = devices.register_verify(body.code)
    if mac:
        if body.name.strip():
            devices.rename(mac, body.name.strip())
        return {"ok": True, "mac": mac}
    if devices.code_exists(body.code):
        raise HTTPException(400, detail="设备未完成令牌绑定，请确认固件已更新")
    raise HTTPException(404, detail="注册码错误或已过期")


class AdminMac(BaseModel):
    mac: str
    name: str = ""

@app.post("/api/admin/rename")
async def admin_rename(body: AdminMac):
    if devices.rename(body.mac, body.name):
        return {"ok": True}
    raise HTTPException(404, detail="设备不存在")

@app.post("/api/admin/ban")
async def admin_ban(body: AdminMac):
    if devices.ban(body.mac):
        return {"ok": True}
    raise HTTPException(404, detail="设备不存在")

@app.post("/api/admin/unban")
async def admin_unban(body: AdminMac):
    if devices.unban(body.mac):
        return {"ok": True}
    raise HTTPException(404, detail="设备不存在")

@app.post("/api/admin/remove")
async def admin_remove(body: AdminMac):
    if devices.remove(body.mac):
        return {"ok": True}
    raise HTTPException(404, detail="设备不存在")

@app.post("/api/admin/reset-token")
async def admin_reset_token(body: AdminMac):
    if devices.reset_token(body.mac):
        return {"ok": True}
    raise HTTPException(404, detail="设备不存在")


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
