"""AuthStick — code generation, approval, and session management."""
import json
import logging
import os
import secrets
import time

logger = logging.getLogger(__name__)

DISPLAY_CODE_TTL = 300       # 5 minutes
DISPLAY_CODE_LENGTH = 6
DEVICE_VERIFY_TTL = 300      # 5 minutes for device registration
DEVICE_VERIFY_LENGTH = 6
SESSION_TTL = 3600           # 1 hour


def _now() -> float:
    return time.time()


class SessionStore:
    def __init__(self):
        self._sessions: dict[str, float] = {}

    def create(self) -> str:
        token = secrets.token_hex(32)
        self._sessions[token] = _now() + SESSION_TTL
        return token

    def validate(self, token: str) -> bool:
        exp = self._sessions.get(token)
        if exp is None:
            return False
        if _now() > exp:
            del self._sessions[token]
            return False
        return True

    def revoke(self, token: str) -> None:
        self._sessions.pop(token, None)

    def gc(self) -> None:
        now = _now()
        expired = [t for t, e in self._sessions.items() if now > e]
        for t in expired:
            del self._sessions[t]


class CodeStore:
    """Display codes for stick-based approval flow."""

    def __init__(self):
        self._pending: dict[str, dict] = {}  # code → {expiry, site_name, approved_by, denied_by, session_token}

    def create(self, site_name: str = "", device: str = "") -> str:
        self.gc()
        for _ in range(10):  # retry on collision
            code = "".join(secrets.choice("0123456789") for _ in range(DISPLAY_CODE_LENGTH))
            if code not in self._pending:
                break
        self._pending[code] = {
            "expiry": _now() + DISPLAY_CODE_TTL,
            "site_name": site_name,
            "device": device,
            "approved_by": None,
            "denied_by": None,
            "session_token": None,
        }
        logger.info("Code created: %s (device=%s)", code, device or "unknown")
        logger.info("Code created: %s (site=%s)", code, site_name or "default")
        return code

    def list_pending(self) -> list[dict]:
        self.gc()
        now = _now()
        result = []
        for code, entry in self._pending.items():
            if entry.get("approved_by") or entry.get("denied_by"):
                continue
            result.append({
                "code": code,
                "site_name": entry.get("site_name", ""),
                "expires_in": max(0, int(entry["expiry"] - now)),
            })
        return result

    def approve(self, code: str, device_mac: str) -> str | None:
        """Approve code, create session token. Returns token or None."""
        self.gc()
        entry = self._pending.get(code.strip())
        if entry is None:
            return None
        if _now() > entry["expiry"]:
            return None
        if entry.get("approved_by") or entry.get("denied_by"):
            return None
        entry["approved_by"] = device_mac
        logger.info("Code %s approved by %s", code, device_mac)
        return code

    def deny(self, code: str, device_mac: str) -> bool:
        self.gc()
        entry = self._pending.get(code.strip())
        if entry is None:
            return False
        if _now() > entry["expiry"]:
            return False
        entry["denied_by"] = device_mac
        logger.info("Code %s denied by %s", code, device_mac)
        return True

    def check_approval(self, code: str) -> str | None:
        """Poll for approval. Returns session_token if approved, None if still pending."""
        self.gc()
        entry = self._pending.get(code.strip())
        if entry is None:
            return None
        if entry.get("session_token"):
            token = entry["session_token"]
            del self._pending[code.strip()]
            return token
        return None

    def set_session_token(self, code: str, token: str) -> None:
        entry = self._pending.get(code.strip())
        if entry:
            entry["session_token"] = token

    def gc(self) -> None:
        now = _now()
        for code in list(self._pending):
            if self._pending[code]["expiry"] < now:
                del self._pending[code]


class DeviceStore:
    """Device registration — untrusted devices must verify before use."""

    def __init__(self, db_path: str = ""):
        self._db_path = db_path or os.path.join(os.path.dirname(__file__), "devices.json")
        self._devices: dict[str, dict] = {}  # mac → {name, registered_at, token}
        self._pending: dict[str, dict] = {}  # verify_code → {mac, expiry, token}
        self._tokens: dict[str, str] = {}    # token → mac
        self._load()
        for mac, d in self._devices.items():
            if "token" in d:
                self._tokens[d["token"]] = mac

    def _load(self) -> None:
        try:
            with open(self._db_path) as f:
                self._devices = json.load(f)
            logger.info("Loaded %d registered devices", len(self._devices))
        except (FileNotFoundError, json.JSONDecodeError):
            self._devices = {}

    def _save(self) -> None:
        with open(self._db_path, "w") as f:
            json.dump(self._devices, f, indent=2)

    def is_registered(self, mac: str) -> bool:
        mac = self._norm_mac(mac)
        return mac in self._devices and bool(self._devices[mac].get("token"))

    def list_registered(self) -> list[dict]:
        return [{"mac": m, **d} for m, d in self._devices.items()]

    def register_begin(self, mac: str) -> dict:
        """Start device registration. Returns {code, expires_in}."""
        self.gc()
        mac = self._norm_mac(mac)
        # Reject if already registered with a token
        if mac in self._devices and self._devices[mac].get("token"):
            return {"error": "device already registered"}
        code = "".join(secrets.choice("0123456789") for _ in range(DEVICE_VERIFY_LENGTH))
        self._pending[code] = {
            "mac": mac,
            "token": "",
            "expiry": _now() + DEVICE_VERIFY_TTL,
        }
        logger.info("Device registration started: %s → code %s", mac, code)
        return {"code": code, "expires_in": DEVICE_VERIFY_TTL}

    def bind_token(self, mac: str, code: str, token: str) -> bool:
        """Bind device token to a pending registration code. Requires code match."""
        mac = self._norm_mac(mac)
        entry = self._pending.get(code.strip())
        if not entry or entry["mac"] != mac:
            return False
        entry["token"] = token
        logger.info("Token bound for %s via code %s", mac, code)
        return True

    def code_exists(self, code: str) -> bool:
        return code.strip() in self._pending

    def register_verify(self, verify_code: str) -> str | None:
        """Admin verifies a device registration code. Returns mac or None."""
        self.gc()
        entry = self._pending.pop(verify_code.strip(), None)
        if entry is None:
            return None
        mac = entry["mac"]
        token = entry.get("token", "")
        # Must have token bound before verification
        if not token:
            # Put it back so admin can retry after device binds token
            self._pending[verify_code.strip()] = entry
            return None
        is_rereg = mac in self._devices
        self._devices[mac] = {
            "name": self._devices[mac].get("name", mac) if is_rereg else mac,
            "registered_at": int(_now()),
            "token": token,
        }
        self._tokens[token] = mac
        self._save()
        logger.info("Device %s: %s", "re-registered" if is_rereg else "registered", mac)
        return mac

    def lookup_by_token(self, token: str) -> str | None:
        """Return MAC for device token, or None."""
        return self._tokens.get(token)

    def get_token(self, mac: str) -> str:
        """Return device token for MAC, or empty string."""
        mac = self._norm_mac(mac)
        return self._devices.get(mac, {}).get("token", "")

    def reset_token(self, mac: str) -> bool:
        """Clear token to force re-registration. Device record preserved."""
        mac = self._norm_mac(mac)
        if mac not in self._devices: return False
        tok = self._devices[mac].get("token", "")
        if tok and tok in self._tokens: del self._tokens[tok]
        self._devices[mac].pop("token", None)
        self._save()
        return True

    def rotate_token(self, mac: str) -> str:
        """Generate new token for device. Old token must be valid."""
        mac = self._norm_mac(mac)
        if mac not in self._devices: return ""
        old = self._devices[mac].get("token", "")
        new = secrets.token_hex(16)
        self._devices[mac]["token"] = new
        if old and old in self._tokens: del self._tokens[old]
        self._tokens[new] = mac
        self._save()
        logger.info("Token rotated for %s", mac)
        return new

    def assign_token(self, mac: str) -> str:
        """Generate and save a token for an existing device."""
        mac = self._norm_mac(mac)
        token = secrets.token_hex(16)
        if mac in self._devices:
            self._devices[mac]["token"] = token
            self._tokens[token] = mac
            self._save()
        return token

    def check_registration(self, mac: str) -> bool:
        """Poll: has this device been registered with a valid token?"""
        return self.is_registered(mac) and not self.is_banned(mac)

    def rename(self, mac: str, name: str) -> bool:
        mac = self._norm_mac(mac)
        if mac not in self._devices: return False
        self._devices[mac]["name"] = name
        self._save()
        return True

    def ban(self, mac: str) -> bool:
        mac = self._norm_mac(mac)
        if mac not in self._devices: return False
        self._devices[mac]["banned"] = True
        self._save()
        return True

    def unban(self, mac: str) -> bool:
        mac = self._norm_mac(mac)
        if mac not in self._devices: return False
        self._devices[mac]["banned"] = False
        self._save()
        return True

    def remove(self, mac: str) -> bool:
        mac = self._norm_mac(mac)
        if mac not in self._devices: return False
        del self._devices[mac]
        self._save()
        return True

    @staticmethod
    def _norm_mac(mac: str) -> str:
        """Normalize MAC to uppercase with colons."""
        mac = mac.upper().replace(":", "").replace("-", "")
        return ":".join(mac[i:i+2] for i in range(0, 12, 2))

    def is_banned(self, mac: str) -> bool:
        mac = self._norm_mac(mac)
        return mac in self._devices and self._devices[mac].get("banned", False)

    def get_name(self, mac: str) -> str:
        mac = self._norm_mac(mac)
        if mac in self._devices:
            return self._devices[mac].get("name", mac)
        return mac

    def gc(self) -> None:
        now = _now()
        for code in list(self._pending):
            if self._pending[code]["expiry"] < now:
                del self._pending[code]
