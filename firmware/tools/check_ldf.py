#!/usr/bin/env python3
"""
check_ldf.py — imita la Library Dependency Finder di PlatformIO 6.1.16 in
modalità "chain" (quella di Display Hub) e verifica che OGNI #include di ogni
sorgente compilato si risolva con i percorsi che PlatformIO darebbe davvero.

Serve a trovare, senza PlatformIO e senza scheda, errori come:
  ml_noise.c: fatal error: crypto/refc/chacha20poly1305.h: No such file
(una libreria usata dai sorgenti di un'altra, ma che la LDF non scopre).

Regole riprodotte (platformio/builder/tools/piolib.py, v6.1.16):
- una libreria viene scoperta quando un file analizzato include un suo header;
- di una libreria si analizzano SOLO gli header inclusi da altri e, per ognuno,
  il .c/.cpp con lo stesso nome nella STESSA cartella (PARSE_SRC_BY_H_NAME);
  i suoi altri sorgenti vengono compilati ma non analizzati;
- "dependencies" di library.json senza owner: risolte per nome tra le
  librerie locali/del framework (mai scaricate: pio pkg install le salta);
- cartelle di include: includeDir (o include/) + src; i percorsi delle
  dipendenze si ereditano in modo transitivo; lib_ignore esclude librerie.

Uso:
  python3 check_ldf.py --project firmware --arduino <core arduino-esp32 2.0.17>
  python3 check_ldf.py --project firmware/loader-full --extra-lib-dir firmware/lib \\
          --global-include firmware/include --arduino <core>
Esce con codice 1 se trova include non risolti.
"""
import argparse
import json
import os
import re
import sys

INC_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
SRC_EXT = (".c", ".cpp", ".cc", ".cxx", ".S")
HDR_EXT = (".h", ".hpp", ".hh", ".hxx", ".inc")


def includes_of(path):
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return []
    # commenti /* */ e // tolti (gli #include commentati non contano)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return [(m.group(1), m.group(2)) for m in INC_RE.finditer(text)]


class Lib:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.name = os.path.basename(self.path)
        self.manifest = {}
        mf = os.path.join(self.path, "library.json")
        if os.path.isfile(mf):
            with open(mf) as f:
                self.manifest = json.load(f)
            self.name = self.manifest.get("name", self.name)
        elif os.path.isfile(os.path.join(self.path, "library.properties")):
            with open(os.path.join(self.path, "library.properties"), errors="replace") as f:
                for line in f:
                    if line.startswith("name="):
                        self.name = line.split("=", 1)[1].strip()
        build = self.manifest.get("build", {})
        src = build.get("srcDir")
        self.src_dir = os.path.join(self.path, src) if src else (
            os.path.join(self.path, "src") if os.path.isdir(os.path.join(self.path, "src")) else self.path)
        inc = build.get("includeDir")
        if inc:
            self.include_dir = os.path.abspath(os.path.join(self.path, inc))
        elif os.path.isdir(os.path.join(self.path, "include")):
            self.include_dir = os.path.join(self.path, "include")
        else:
            self.include_dir = None
        self.inc_dirs = [d for d in [self.include_dir, self.src_dir] if d]
        # -I delle build flags della libreria (relative alla libreria)
        for fl in build.get("flags", []):
            for m in re.finditer(r"-I\s*(\S+)", fl):
                self.inc_dirs.append(os.path.abspath(os.path.join(self.path, m.group(1))))
        self.deps = []            # librerie da cui dipende
        self.dependent = False
        self.scanned = set()

    def contains(self, p):
        return os.path.abspath(p).startswith(self.path + os.sep)

    def dependencies(self):
        d = self.manifest.get("dependencies")
        if isinstance(d, dict):
            return [{"name": k} for k in d]
        return [x if isinstance(x, dict) else {"name": x} for x in (d or [])]

    def sources(self):
        """File compilati (src_filter di library.json, semplificato)."""
        filt = self.manifest.get("build", {}).get("srcFilter")
        out = []
        for root, dirs, files in os.walk(self.src_dir):
            dirs[:] = [x for x in dirs if x not in ("examples", "example", "test", "tests")]
            for fn in files:
                if fn.endswith(SRC_EXT):
                    rel = os.path.relpath(os.path.join(root, fn), self.src_dir)
                    if filt and not _filter_ok(rel, filt):
                        continue
                    out.append(os.path.join(root, fn))
        return out


def _filter_ok(rel, filt):
    import fnmatch
    ok = False
    for f in filt if isinstance(filt, list) else filt.split():
        sign, pat = f[0], f[2:-1]
        if fnmatch.fnmatch(rel, pat) or fnmatch.fnmatch(rel, pat.rstrip("/") + "/*"):
            ok = sign == "+"
    return ok


class Ldf:
    def __init__(self, libs, global_dirs, system_dirs):
        self.libs = libs
        self.global_dirs = global_dirs
        self.system_dirs = system_dirs  # core/SDK: sempre nel percorso

    def all_inc_dirs(self):
        dirs = list(self.global_dirs)
        for lb in self.libs:
            dirs += lb.inc_dirs
        return dirs + self.system_dirs

    def resolve(self, kind, name, from_file, dirs):
        cands = ([os.path.dirname(from_file)] if kind == '"' else []) + dirs
        for d in cands:
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return os.path.abspath(p)
        return None

    def owner(self, path):
        for lb in self.libs:
            if lb.contains(path):
                return lb
        return None

    def depend_on(self, holder, lb, search_files):
        if holder is not None and holder is not lb and lb not in holder.deps:
            holder.deps.append(lb)
        lb.dependent = True
        self.search(lb, search_files)

    def process_deps(self, lb):
        for dep in lb.dependencies():
            if dep.get("owner"):
                continue
            for other in self.libs:
                if other.name == dep["name"]:
                    if other not in lb.deps:
                        lb.deps.append(other)
                    other.dependent = True
                    self.search(other, [])
                    break

    def search(self, lb, search_files, project=False):
        """search_deps_recursive + get_implicit_includes."""
        if lb is not None and not getattr(lb, "_deps_done", False):
            lb._deps_done = True
            self.process_deps(lb)
        own_dirs = (lb.inc_dirs if lb else [])
        dirs = own_dirs + self.all_inc_dirs()
        queue = list(search_files)
        found_by_lib = {}
        scanned = lb.scanned if lb else self._proj_scanned
        while queue:
            f = queue.pop(0)
            if f in scanned:
                continue
            scanned.add(f)
            for kind, name in includes_of(f):
                p = self.resolve(kind, name, f, dirs)
                if not p:
                    continue
                inside = (lb.contains(p) if lb else self._in_project(p))
                if inside and p not in scanned:
                    queue.append(p)
                if p.endswith(HDR_EXT):
                    stem = p[: p.rindex(".")]
                    for ext in (".c", ".cpp"):
                        if os.path.isfile(stem + ext):
                            src = stem + ext
                            if inside and src not in scanned:
                                queue.append(src)
                            elif not inside:
                                other = self.owner(src)
                                if other:
                                    found_by_lib.setdefault(other, []).append(src)
                other = self.owner(p)
                if other and other is not lb:
                    found_by_lib.setdefault(other, []).append(p)
        for other, files in found_by_lib.items():
            self.depend_on(lb, other, files)

    def _in_project(self, p):
        return any(p.startswith(d + os.sep) for d in self._project_dirs)

    def run(self, project_sources, project_dirs):
        self._proj_scanned = set()
        self._project_dirs = [os.path.abspath(d) for d in project_dirs]
        self.search(None, project_sources, project=True)

    def cpppath(self, lb, seen=None):
        seen = seen or set()
        if lb in seen:
            return []
        seen.add(lb)
        dirs = list(lb.inc_dirs)
        for d in lb.deps:
            dirs += self.cpppath(d, seen)
        return dirs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True, help="cartella con src/ (e lib/)")
    ap.add_argument("--extra-lib-dir", action="append", default=[], help="come lib_extra_dirs")
    ap.add_argument("--global-include", action="append", default=[],
                    help="come -I nelle build_flags (valgono per TUTTE le librerie)")
    ap.add_argument("--arduino", required=True, help="core arduino-esp32 2.0.17 (cartella con cores/, libraries/, tools/sdk)")
    ap.add_argument("--ignore", action="append", default=["BLE"], help="come lib_ignore")
    ap.add_argument("--show-graph", action="store_true")
    a = ap.parse_args()

    proj = os.path.abspath(a.project)
    lib_dirs = [os.path.join(proj, "lib")] + [os.path.abspath(d) for d in a.extra_lib_dir] + \
        [os.path.join(a.arduino, "libraries")]
    libs = []
    for ld in lib_dirs:
        if not os.path.isdir(ld):
            continue
        for name in sorted(os.listdir(ld)):
            p = os.path.join(ld, name)
            if os.path.isdir(p) and not name.startswith("."):
                lb = Lib(p)
                if lb.name not in a.ignore and name not in a.ignore:
                    libs.append(lb)

    sdk = os.path.join(a.arduino, "tools", "sdk", "esp32s3")
    system_dirs = [os.path.join(a.arduino, "cores", "esp32"), os.path.join(a.arduino, "variants", "esp32s3"),
                   os.path.join(sdk, "qio_opi", "include")]
    for root, dirs, _ in os.walk(os.path.join(sdk, "include")):
        system_dirs.append(root)
    # PlatformIO dà la cartella include/ del progetto SOLO ai sorgenti del
    # progetto (src/), non alle librerie: a quelle arrivano solo i -I delle
    # build_flags. (Es. LVGL non trova include/lv_conf.h senza "-I include".)
    proj_inc = os.path.join(proj, "include")
    global_dirs = [os.path.abspath(d) for d in a.global_include]

    ldf = Ldf(libs, global_dirs + [proj_inc], system_dirs)
    proj_src = []
    for root, _, files in os.walk(os.path.join(proj, "src")):
        proj_src += [os.path.join(root, f) for f in files if f.endswith(SRC_EXT)]
    ldf.run(proj_src, [os.path.join(proj, "src"), os.path.join(proj, "include")])

    built = [lb for lb in libs if lb.dependent]
    problems = 0
    reported = set()
    sys_prefixes = tuple(os.path.abspath(d) + os.sep for d in system_dirs)
    for lb in built:
        dirs = [x for x in ldf.cpppath(lb)] + global_dirs + system_dirs
        # Come il compilatore: si seguono anche gli header inclusi (a catena),
        # compilati nel contesto di QUESTA libreria. Gli header di sistema no.
        queue = list(lb.sources())
        seen = set(queue)
        while queue:
            src = queue.pop()
            for kind, name in includes_of(src):
                p = ldf.resolve(kind, name, src, dirs)
                if p:
                    if p not in seen and not p.startswith(sys_prefixes) and p.endswith(HDR_EXT):
                        seen.add(p)
                        queue.append(p)
                    continue
                # Solo il caso che rompe (o falsa) la build: l'header ESISTE in
                # una libreria o in include/ del progetto, ma fuori dalla vista
                # di chi lo include. (Include introvabili ovunque sono in #if
                # per altre piattaforme, es. nRF in NimBLE: il compilatore li salta.)
                where = [l.name for l in libs if ldf.resolve(kind, name, os.path.join(l.src_dir, "x"), l.inc_dirs)]
                if os.path.isfile(os.path.join(proj_inc, name)):
                    where.append("include/ del progetto (manca -I nelle build_flags)")
                key = (lb.name, name)
                if where and key not in reported:
                    reported.add(key)
                    problems += 1
                    print("NON RISOLTO  %-16s %s: #include %s%s%s   (è in: %s)" % (
                        lb.name, os.path.relpath(src, lb.path), kind, name, '"' if kind == '"' else ">",
                        ", ".join(where)))
    if a.show_graph:
        for lb in built:
            print("  %-22s -> %s" % (lb.name, ", ".join(d.name for d in lb.deps) or "-"))
    print("%d librerie compilate, %d include non risolti" % (len(built), problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
