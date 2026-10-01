"""
Autenticazione della web UI: login con utente e password, sessione in un
cookie firmato ("Ricordami" = cookie che dura DH_SESSION_DAYS giorni).

Configurazione dal container (docker-compose.yml / .env / variabili dello stack
in Portainer):
  DH_AUTH_USER            utente (default "admin")
  DH_AUTH_PASSWORD_HASH   hash della password (consigliato), vedi sotto
  DH_AUTH_PASSWORD        in alternativa la password in chiaro
  DH_SECRET_KEY           chiave per firmare i cookie (se manca: generata e
                          salvata in data/secret_key, così le sessioni
                          sopravvivono ai riavvii)
  DH_SESSION_DAYS         durata di "Ricordami" (default 30)
  DH_ESP_VIA_PROXY        1 = le API dei display accettano anche richieste
                          arrivate dal reverse proxy (default 0, vedi sotto)
Senza password configurata l'autenticazione è DISATTIVATA (come prima), con un
avviso nel log.

Hash: python -m app.core.auth hash   (chiede la password, stampa l'hash)
Formato pbkdf2_sha256:<iterazioni>:<sale>:<hash>, SENZA "$": docker compose e
Portainer interpretano "$" come variabile e rovinerebbero gli hash standard.

Cosa è protetto: tutte le pagine e le API del browser. NON le API dei display
(/api/esp/*): il firmware non può fare login. Quelle però rispondono solo alle
richieste DIRETTE (porta 12000 in LAN o via VPN, come fa il firmware): se una
richiesta arriva dal reverse proxy (NPM aggiunge X-Forwarded-For) viene
rifiutata, perché /api/esp/config restituisce le configurazioni dei display
con password e chiavi.

Protezioni: cookie HttpOnly, SameSite=Lax (niente richieste da altri siti con
il tuo cookie), Secure quando la pagina è in HTTPS; 5 tentativi sbagliati in 15
minuti dallo stesso IP -> blocco di 15 minuti; confronto a tempo costante;
cambiando la password tutte le sessioni aperte decadono.
"""
import getpass
import hashlib
import hmac
import logging
import os
import secrets
import sys
import threading
import time
from datetime import timedelta
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)

_DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data")
MAX_FAILURES = 5
FAILURE_WINDOW_S = 15 * 60
LOCKOUT_S = 15 * 60
PBKDF2_ITERATIONS = 390000


# ---------------------------------------------------------------- hash
def hash_password(password: str, iterations: int = PBKDF2_ITERATIONS) -> str:
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), iterations).hex()
    return f"pbkdf2_sha256:{iterations}:{salt}:{digest}"


def verify_hash(stored: str, password: str) -> bool:
    try:
        algo, iterations, salt, digest = stored.strip().split(":")
        if algo != "pbkdf2_sha256":
            return False
        calc = hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), int(iterations)).hex()
        return hmac.compare_digest(calc, digest)
    except (ValueError, TypeError):
        return False


# ---------------------------------------------------------------- configurazione
class AuthConfig:
    def __init__(self, env=None):
        env = os.environ if env is None else env
        self.user = (env.get("DH_AUTH_USER") or "admin").strip()
        self.password_hash = (env.get("DH_AUTH_PASSWORD_HASH") or "").strip()
        self.password = env.get("DH_AUTH_PASSWORD") or ""
        self.enabled = bool(self.password_hash or self.password)
        try:
            self.session_days = max(1, int(env.get("DH_SESSION_DAYS") or 30))
        except ValueError:
            self.session_days = 30
        self.esp_via_proxy = (env.get("DH_ESP_VIA_PROXY") or "0").strip() in ("1", "true", "yes")
        self.secret_key = (env.get("DH_SECRET_KEY") or "").strip() or _persistent_secret()
        if self.password_hash and not self.password_hash.startswith("pbkdf2_sha256:"):
            logger.error("DH_AUTH_PASSWORD_HASH non valido (atteso pbkdf2_sha256:...): login impossibile")

    def check(self, user: str, password: str) -> bool:
        user_ok = hmac.compare_digest((user or "").strip().encode(), self.user.encode())
        if self.password_hash:
            pw_ok = verify_hash(self.password_hash, password or "")
        else:
            pw_ok = hmac.compare_digest((password or "").encode(), self.password.encode())
        return user_ok and pw_ok

    def fingerprint(self) -> str:
        """Cambia se cambiano utente o password: le sessioni vecchie decadono."""
        material = f"{self.user}\0{self.password_hash or self.password}".encode()
        return hmac.new(self.secret_key.encode(), material, hashlib.sha256).hexdigest()[:24]


def _persistent_secret() -> str:
    path = os.path.join(_DATA_DIR, "secret_key")
    try:
        with open(path) as f:
            key = f.read().strip()
            if len(key) >= 32:
                return key
    except OSError:
        pass
    key = secrets.token_hex(32)
    try:
        os.makedirs(_DATA_DIR, exist_ok=True)
        with open(path, "w") as f:
            f.write(key)
        os.chmod(path, 0o600)
    except OSError as exc:
        logger.warning("secret_key non salvata (%s): le sessioni non sopravvivranno a un riavvio", exc)
    return key


# ---------------------------------------------------------------- tentativi sbagliati
class LoginThrottle:
    def __init__(self):
        self._lock = threading.Lock()
        self._fails: Dict[str, List[float]] = {}

    def locked_for(self, ip: str, now: Optional[float] = None) -> int:
        now = now or time.time()
        with self._lock:
            fails = [t for t in self._fails.get(ip, []) if now - t < FAILURE_WINDOW_S + LOCKOUT_S]
            self._fails[ip] = fails
            recent = [t for t in fails if now - t < FAILURE_WINDOW_S]
            if len(recent) >= MAX_FAILURES:
                return int(max(0, fails[-1] + LOCKOUT_S - now)) or 1
            return 0

    def fail(self, ip: str, now: Optional[float] = None):
        with self._lock:
            self._fails.setdefault(ip, []).append(now or time.time())
            if len(self._fails) > 5000:
                self._fails.clear()

    def success(self, ip: str):
        with self._lock:
            self._fails.pop(ip, None)


# ---------------------------------------------------------------- Flask
def init_app(app, config: Optional[AuthConfig] = None):
    from flask import jsonify, redirect, render_template, request, session, url_for
    from flask.sessions import SecureCookieSessionInterface
    from werkzeug.middleware.proxy_fix import ProxyFix

    cfg = config or AuthConfig()
    throttle = LoginThrottle()
    app.config["DH_AUTH"] = cfg
    app.secret_key = cfg.secret_key
    app.config.update(SESSION_COOKIE_NAME="dh_session", SESSION_COOKIE_HTTPONLY=True,
                      SESSION_COOKIE_SAMESITE="Lax",
                      PERMANENT_SESSION_LIFETIME=timedelta(days=cfg.session_days))
    # Dietro NPM: schema (https) e IP reali del client dalle intestazioni del proxy.
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1)

    class _Session(SecureCookieSessionInterface):
        def get_cookie_secure(self, app_):
            return request.is_secure  # Secure in HTTPS (NPM), no in http://IP:12000 in LAN

    app.session_interface = _Session()

    if cfg.enabled:
        logger.info("Autenticazione attiva (utente '%s', sessioni di %d giorni)", cfg.user, cfg.session_days)
    else:
        logger.warning("Autenticazione DISATTIVATA: imposta DH_AUTH_PASSWORD_HASH (o DH_AUTH_PASSWORD) "
                       "nel container. Chiunque raggiunga la pagina vede configurazioni, password e chiavi.")

    def via_proxy() -> bool:
        return "X-Forwarded-For" in request.headers

    def logged_in() -> bool:
        return session.get("u") == cfg.user and session.get("f") == cfg.fingerprint()

    @app.context_processor
    def _auth_vars():
        return {"auth_enabled": cfg.enabled, "auth_user": cfg.user if cfg.enabled and logged_in() else None}

    @app.before_request
    def _guard():
        path = request.path
        if path.startswith("/static/") or path in ("/login", "/logout", "/favicon.ico"):
            return None
        if path.startswith("/api/esp/"):
            if via_proxy() and not cfg.esp_via_proxy:
                return jsonify({"error": "API dei display raggiungibili solo direttamente (porta 12000), "
                                         "non dal reverse proxy"}), 403
            return None
        if not cfg.enabled or logged_in():
            return None
        if path.startswith("/api/"):
            return jsonify({"error": "login richiesto", "login": "/login"}), 401
        return redirect(url_for("auth_login", next=request.full_path.rstrip("?")))

    def safe_next(target: str) -> str:
        # Solo percorsi interni: niente redirect verso altri siti ("//x", "http://x").
        if not target or not target.startswith("/") or target.startswith("//") or "\\" in target:
            return "/"
        return target

    @app.route("/login", methods=["GET", "POST"], endpoint="auth_login")
    def login():
        nxt = safe_next(request.values.get("next", "/"))
        if not cfg.enabled or logged_in():
            return redirect(nxt)
        error = None
        ip = request.remote_addr or "?"
        if request.method == "POST":
            wait = throttle.locked_for(ip)
            if wait:
                error = f"Troppi tentativi sbagliati: riprova tra {max(1, wait // 60)} min."
            elif cfg.check(request.form.get("username", ""), request.form.get("password", "")):
                throttle.success(ip)
                session.clear()
                session.permanent = request.form.get("remember") == "1"
                session["u"] = cfg.user
                session["f"] = cfg.fingerprint()
                logger.info("Accesso alla web UI da %s", ip)
                return redirect(nxt)
            else:
                throttle.fail(ip)
                time.sleep(0.5)  # rallenta i tentativi automatici
                logger.warning("Accesso NON riuscito alla web UI da %s", ip)
                wait = throttle.locked_for(ip)
                error = (f"Troppi tentativi sbagliati: riprova tra {max(1, wait // 60)} min." if wait
                         else "Utente o password non corretti.")
        return render_template("login.html", error=error, next=nxt, session_days=cfg.session_days), \
            (429 if error and "Troppi" in error else 200)

    @app.route("/logout", methods=["POST", "GET"])
    def logout():
        session.clear()
        return redirect(url_for("auth_login"))


# ---------------------------------------------------------------- riga di comando
if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "hash":
        pw = getpass.getpass("Password: ")
        if pw != getpass.getpass("Ripeti la password: "):
            print("Le due password non coincidono.")
            sys.exit(1)
        if len(pw) < 8:
            print("Attenzione: password più corta di 8 caratteri.")
        print("\nMetti questa riga nel file .env (o tra le variabili dello stack in Portainer):\n")
        print(f"DH_AUTH_PASSWORD_HASH={hash_password(pw)}")
    else:
        print("Uso: python -m app.core.auth hash")
