"""Autenticazione della web UI. Esecuzione: python3 tests/test_auth.py"""
import os
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import auth  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


auth._DATA_DIR = tempfile.mkdtemp()
PROXY = {"X-Forwarded-For": "93.40.1.2", "X-Forwarded-Proto": "https", "X-Forwarded-Host": "hub.example.it"}


def make(env):
    with mock.patch.dict(os.environ, env, clear=False):
        for k in ("DH_AUTH_PASSWORD", "DH_AUTH_PASSWORD_HASH"):
            if k not in env:
                os.environ.pop(k, None)
        from app.main import create_app
        app = create_app()
    app.config["TESTING"] = True
    return app, app.test_client()


with mock.patch.object(auth.time, "sleep", lambda s: None):
    print("== Senza password: accesso libero (come prima) ==")
    app, c = make({})
    check(c.get("/").status_code == 200 and c.get("/api/pages").status_code == 200, "pagina e API libere")
    check(not app.config["DH_AUTH"].enabled, "autenticazione disattivata")

    print("\n== Con password ==")
    h = auth.hash_password("Segreta-123", iterations=1000)
    check(h.startswith("pbkdf2_sha256:") and "$" not in h, "hash senza '$' (compose/Portainer lo rovinerebbero)")
    check(auth.verify_hash(h, "Segreta-123") and not auth.verify_hash(h, "segreta-123"), "verifica dell'hash")
    app, c = make({"DH_AUTH_USER": "donato", "DH_AUTH_PASSWORD_HASH": h, "DH_SECRET_KEY": "k" * 64})
    r = c.get("/")
    check(r.status_code == 302 and "/login?next=/" in r.headers["Location"], "pagina -> login")
    r = c.get("/api/pages")
    check(r.status_code == 401 and r.get_json()["login"] == "/login", "API del browser: 401")
    check(c.get("/static/css/style.css").status_code == 200, "file statici (stile del login) liberi")
    check(c.get("/login").status_code == 200 and b"Ricordami" in c.get("/login").data, "pagina di login")

    print("\n== API dei display ==")
    r = c.get("/api/esp/poll?device_id=sala-01")
    check(r.status_code == 200 and "pages" in r.get_json(), "display in diretta (porta 12000): senza login")
    r = c.get("/api/esp/config?device_id=sala-01", headers=PROXY)
    check(r.status_code == 403, "dal reverse proxy: rifiutate (configurazioni con password)")

    print("\n== Login ==")
    r = c.post("/login", data={"username": "donato", "password": "sbagliata", "next": "/"})
    check(r.status_code == 200 and "non corretti".encode() in r.data, "password sbagliata: messaggio")
    r = c.post("/login", data={"username": "donato", "password": "Segreta-123", "next": "/#/firmware", "remember": "1"},
               environ_base={"REMOTE_ADDR": "192.168.1.50"})
    check(r.status_code == 302 and r.headers["Location"].endswith("/#/firmware"), "login riuscito: ritorno alla sezione")
    cookie = r.headers.get("Set-Cookie", "")
    check("dh_session=" in cookie and "HttpOnly" in cookie and "SameSite=Lax" in cookie, "cookie HttpOnly, SameSite=Lax")
    check("Expires=" in cookie and "Secure" not in cookie, "Ricordami: cookie che dura; in http diretto niente Secure")
    check(c.get("/api/pages").status_code == 200 and "donato".encode() in c.get("/").data, "dopo il login: API e pagina, utente in testata")
    c.post("/logout")
    check(c.get("/api/pages").status_code == 401, "Esci: sessione chiusa")

    print("\n== Dietro NPM (HTTPS) ==")
    c2 = app.test_client()
    r = c2.post("/login", data={"username": "donato", "password": "Segreta-123", "next": "/"}, headers=PROXY)
    cookie = r.headers.get("Set-Cookie", "")
    check("Secure" in cookie and "Expires=" not in cookie, "in HTTPS: cookie Secure; senza Ricordami, cookie di sessione")
    check(c2.get("/api/pages", headers=PROXY).status_code == 200, "API via proxy dopo il login")
    check(c2.get("/api/esp/poll?device_id=x", headers=PROXY).status_code == 403, "anche da loggati, /api/esp non passa dal proxy")

    print("\n== Rinvii solo interni ==")
    for nxt in ("//evil.example", "https://evil.example", "/\\evil"):
        c3 = app.test_client()
        r = c3.post("/login", data={"username": "donato", "password": "Segreta-123", "next": nxt},
                    environ_base={"REMOTE_ADDR": "192.168.1.51"})
        check(r.headers["Location"] in ("/", "http://localhost/"), "next=%s -> /" % nxt)

    print("\n== Tentativi sbagliati ==")
    c4 = app.test_client()
    for i in range(5):
        c4.post("/login", data={"username": "donato", "password": "x%d" % i}, environ_base={"REMOTE_ADDR": "10.0.0.9"})
    r = c4.post("/login", data={"username": "donato", "password": "Segreta-123"}, environ_base={"REMOTE_ADDR": "10.0.0.9"})
    check(r.status_code == 429 and "Troppi".encode() in r.data, "5 errori: bloccato anche con la password giusta")
    r = c4.post("/login", data={"username": "donato", "password": "Segreta-123"}, environ_base={"REMOTE_ADDR": "10.0.0.10"})
    check(r.status_code == 302, "un altro IP non è bloccato")
    t = auth.LoginThrottle()
    for i in range(5):
        t.fail("ip", now=1000 + i)
    check(t.locked_for("ip", now=1100) > 0 and t.locked_for("ip", now=1004 + auth.LOCKOUT_S + 1) == 0, "il blocco scade dopo 15 minuti")

    print("\n== Cambio password ==")
    c5 = app.test_client()
    c5.post("/login", data={"username": "donato", "password": "Segreta-123", "remember": "1"}, environ_base={"REMOTE_ADDR": "192.168.1.52"})
    old_cookie = c5.get_cookie("dh_session").value
    app2, c6 = make({"DH_AUTH_USER": "donato", "DH_AUTH_PASSWORD": "Nuova-456", "DH_SECRET_KEY": "k" * 64})
    c6.set_cookie("dh_session", old_cookie)
    check(c6.get("/api/pages").status_code == 401, "password cambiata: le sessioni vecchie decadono")
    check(c6.post("/login", data={"username": "donato", "password": "Nuova-456"}).status_code == 302, "password in chiaro (senza hash) accettata")

    print("\n== Chiave dei cookie ==")
    k1 = auth.AuthConfig({"DH_AUTH_PASSWORD": "x"}).secret_key
    k2 = auth.AuthConfig({"DH_AUTH_PASSWORD": "x"}).secret_key
    check(len(k1) >= 32 and k1 == k2 and os.path.exists(os.path.join(auth._DATA_DIR, "secret_key")),
          "senza DH_SECRET_KEY: generata e conservata (sessioni valide dopo un riavvio)")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
