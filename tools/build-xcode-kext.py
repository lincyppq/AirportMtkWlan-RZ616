#!/usr/bin/env python3
"""Build one kext target of an Xcode project with only the Command Line Tools.

Reads the target's Sources phase, preprocessor definitions, prefix header and
Info.plist from project.pbxproj, compiles every source as C++ (the projects set
GCC_INPUT_FILETYPE = sourcecode.cpp.cpp), generates kmod_info and links a
kext bundle against MacKernelSDK.

  build-xcode-kext.py <project dir> <target> <Debug|Release> <out dir>
                      [--sdk <MacKernelSDK dir>] [--exclude-dir <name>]...
"""
import argparse, json, os, plistlib, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument("project")
ap.add_argument("target")
ap.add_argument("config", choices=["Debug", "Release"])
ap.add_argument("out")
ap.add_argument("--sdk")
ap.add_argument("--exclude-dir", action="append", default=["MacKernelSDK", ".git", "build"])
ap.add_argument("--define", action="append", default=[])
args = ap.parse_args()

root = os.path.abspath(args.project)
manifest = None
if args.target.endswith(".json"):
    # Targets without an Xcode project: explicit sources and settings.
    manifest = json.load(open(os.path.join(root, args.target)))
else:
    pbx = [f for f in os.listdir(root) if f.endswith(".xcodeproj")][0]
    s = open(os.path.join(root, pbx, "project.pbxproj"), encoding="utf-8").read()


def obj(oid):
    m = re.search(r"\n\t\t" + oid + r" /\*[^*]*\*/ = \{(.*?)\n\t\t\};", s, re.S)
    if not m:
        m = re.search(r"\n\t\t" + oid + r" = \{(.*?)\n\t\t\};", s, re.S)
    return m.group(1) if m else ""


def unescape(v):
    # pbxproj string escapes, then the shell escapes Xcode applies to -D.
    v = v.strip()
    if v.startswith('"') and v.endswith('"'):
        v = v[1:-1].replace('\\\\', '\x00').replace('\\"', '"').replace('\x00', '\\')
    return v.replace('\\"', '"')


def settings(cfg_body):
    bs = re.search(r"buildSettings = \{(.*)\};", cfg_body, re.S).group(1)
    out = {}
    for m in re.finditer(r"\n\t*([A-Z0-9_\[\]=*\"]+) = (\((.*?)\)|\"(.*?)\"|([^;]*));", bs, re.S):
        key = m.group(1).strip('"')
        if m.group(3) is not None:
            out[key] = [unescape(v) for v in m.group(3).split(",\n") if v.strip()]
        else:
            out[key] = (m.group(4) if m.group(4) is not None else m.group(5)).strip()
    return out


def config_settings(list_id):
    lb = obj(list_id)
    for cid, name in re.findall(r"(\w{24}) /\* (\w+) \*/", lb):
        if name == args.config:
            return settings(obj(cid))
    return {}


if manifest:
    sources = [os.path.join(root, x) for x in manifest["sources"]]
    defines = manifest["defines"] + args.define
    module = manifest["module"]
    version = manifest.get("version", "1.0.0")
    pch = manifest.get("prefix_header")
    infoplist = manifest["info_plist"]
    product = manifest["product"]
    deploy = manifest.get("deployment_target", "10.12")
    sysinc = manifest.get("system_header_search_paths", [])
    hdr_roots = [os.path.join(root, x) for x in manifest.get("header_roots", ["."])]
else:
    hdr_roots = [root]
    tm = re.search(r"\n\t\t(\w{24}) /\* " + re.escape(args.target) + r" \*/ = \{\n\t\t\tisa = PBXNativeTarget;", s)
    if not tm:
        sys.exit("target not found: " + args.target)
    tbody = obj(tm.group(1))
    tset = config_settings(re.search(r"buildConfigurationList = (\w+)", tbody).group(1))
    proj = re.search(r"\n\t\t(\w{24}) /\* Project object \*/ = \{(.*?)\n\t\t\};", s, re.S).group(2)
    pset = config_settings(re.search(r"buildConfigurationList = (\w+)", proj).group(1))

    # Sources: build file -> file reference -> path (resolved by searching the tree).
    src_phase = [p for p, n in re.findall(r"(\w{24}) /\* (\w+) \*/", re.search(r"buildPhases = \((.*?)\);", tbody, re.S).group(1)) if n == "Sources"][0]
    names = []
    for bf in re.findall(r"(\w{24}) /\* .*? in Sources \*/", obj(src_phase)):
        ref = re.search(r"fileRef = (\w{24})", obj(bf))
        if not ref:
            continue
        rb = obj(ref.group(1))
        p = re.search(r"path = (\"[^\"]+\"|[^;]+);", rb)
        if p:
            names.append(p.group(1).strip('"'))

    tree = {}
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in args.exclude_dir and not x.endswith(".xcodeproj")]
        for f in files:
            tree.setdefault(f, []).append(os.path.join(d, f))
    sources = []
    for n in names:
        base = os.path.basename(n)
        cands = tree.get(base, [])
        cands = [c for c in cands if c.endswith(n)] or cands
        if len(cands) != 1:
            sys.exit("cannot resolve source %s: %s" % (n, cands))
        if cands[0] not in sources:  # Xcode ignores repeated build files
            sources.append(cands[0])


    def expand(v, extra):
        env = dict(pset); env.update(tset); env.update(extra)
        for _ in range(4):
            v = re.sub(r"\$\((\w+)\)|\$\{(\w+)\}", lambda m: str(env.get(m.group(1) or m.group(2), "")) if not isinstance(env.get(m.group(1) or m.group(2), ""), list) else "", v)
        return v


    def getl(key):
        v = tset.get(key, pset.get(key, []))
        if isinstance(v, str):
            v = [v]
        res = []
        for x in v:
            if x == "$(inherited)":
                pv = pset.get(key, [])
                res += [pv] if isinstance(pv, str) else [y for y in pv if y != "$(inherited)"]
            else:
                res.append(x)
        return res


    extra = {"PROJECT_DIR": root, "SRCROOT": root, "TARGET_NAME": args.target, "MODULE_VERSION": tset.get("MODULE_VERSION", pset.get("MODULE_VERSION", "1.0.0")), "GIT_COMMIT": ""}
    defines = [expand(d, extra) for d in getl("GCC_PREPROCESSOR_DEFINITIONS")] + args.define
    module = expand(tset.get("MODULE_NAME", tset.get("PRODUCT_BUNDLE_IDENTIFIER")), extra)
    version = extra["MODULE_VERSION"]
    pch = tset.get("GCC_PREFIX_HEADER")
    infoplist = expand(tset["INFOPLIST_FILE"], extra)
    product = expand(tset.get("PRODUCT_NAME", args.target), extra)
    deploy = tset.get("MACOSX_DEPLOYMENT_TARGET", pset.get("MACOSX_DEPLOYMENT_TARGET", "10.12"))
    sysinc = expand(tset.get("SYSTEM_HEADER_SEARCH_PATHS", ""), extra).split()

sdk = os.path.abspath(args.sdk or os.path.join(root, "MacKernelSDK"))
sdkh = os.path.join(sdk, "Headers")
out = os.path.abspath(args.out)
objd = os.path.join(out, "obj")
os.makedirs(objd, exist_ok=True)

# Xcode header map: every directory with headers is reachable by bare name,
# except ABI-specific directories that are only meant for relative includes.
hdirs = set()
for d, dirs, files in (w for hr in hdr_roots for w in os.walk(hr)):
    dirs[:] = [x for x in dirs if x not in args.exclude_dir and not x.endswith(".xcodeproj") and not re.match(r"macOS\d+$", x)]
    if any(f.endswith((".h", ".hpp")) for f in files):
        hdirs.add(d)

flags = ["-x", "c++", "-std=gnu++17", "-arch", "x86_64", "-mmacosx-version-min=" + deploy,
         "-mkernel", "-fapple-kext", "-nostdinc", "-nostdinc++", "-fno-builtin", "-fno-common",
         "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-mno-red-zone", "-g",
         "-O0" if args.config == "Debug" else "-O1", "-Wno-everything",
         "-DKERNEL", "-DKERNEL_PRIVATE", "-DDRIVER_PRIVATE", "-DAPPLE", "-DNeXT"]
if args.config == "Debug":
    flags.append("-DDEBUG=1")
for d in defines:
    flags.append("-D" + d)
if pch:
    flags += ["-include", os.path.join(root, pch)]
for d in sysinc:
    flags += ["-isystem", os.path.join(root, d)]
flags += ["-I" + sdkh, "-I" + os.path.join(sdkh, "IOKit")]
for d in sorted(hdirs):
    flags += ["-iquote", d]


def compile_one(src):
    o = os.path.join(objd, os.path.relpath(src, root).replace("/", "_") + ".o")
    r = subprocess.run(["clang"] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
    return src, o, r.returncode, r.stderr


errors = 0
objs = []
with ThreadPoolExecutor(os.cpu_count()) as ex:
    for src, o, rc, err in ex.map(compile_one, sources):
        if rc:
            errors += 1
            print("FAILED", os.path.relpath(src, root))
            print("\n".join(l for l in err.splitlines() if "error" in l)[:3000])
        objs.append(o)
if errors:
    sys.exit("%d files failed" % errors)

info_c = os.path.join(objd, "kmod_info.c")
open(info_c, "w").write('''#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(%s, "%s", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
''' % (module, version))
subprocess.run(["clang", "-x", "c", "-arch", "x86_64", "-mkernel", "-nostdinc", "-DKERNEL", "-I" + sdkh,
                "-c", info_c, "-o", info_c + ".o"], check=True)

clt = "/Library/Developer/CommandLineTools"
cc_kext = sorted(subprocess.run(["sh", "-c", "ls %s/usr/lib/clang/*/lib/darwin/libclang_rt.cc_kext.a" % clt], capture_output=True, text=True).stdout.split())[-1]
kmodcxx = sorted(subprocess.run(["sh", "-c", "ls %s/SDKs/MacOSX15*.sdk/usr/lib/libkmodc++.a" % clt], capture_output=True, text=True).stdout.split())[-1]
kext = os.path.join(out, product + ".kext")
subprocess.run(["rm", "-rf", kext], check=True)
os.makedirs(os.path.join(kext, "Contents", "MacOS"))
binary = os.path.join(kext, "Contents", "MacOS", product)
r = subprocess.run(["clang", "-arch", "x86_64", "-mmacosx-version-min=" + deploy, "-nostdlib", "-Xlinker", "-kext",
                    "-Xlinker", "-export_dynamic"] + objs + [info_c + ".o", os.path.join(sdk, "Library/x86_64/libkmod.a"),
                    kmodcxx, cc_kext, "-o", binary], capture_output=True, text=True)
if r.returncode:
    sys.exit("link failed:\n" + r.stderr[-3000:])

pl = plistlib.load(open(os.path.join(root, infoplist), "rb"))
subst = {"EXECUTABLE_NAME": product, "PRODUCT_NAME": product, "PRODUCT_BUNDLE_IDENTIFIER": module,
         "MODULE_VERSION": version, "CURRENT_PROJECT_VERSION": version, "DEVELOPMENT_LANGUAGE": "en",
         "PRODUCT_BUNDLE_PACKAGE_TYPE": "KEXT", "MARKETING_VERSION": version}


def fix(v):
    if isinstance(v, str):
        return re.sub(r"\$\((\w+)\)", lambda m: subst.get(m.group(1), m.group(0)), v)
    if isinstance(v, dict):
        return {k: fix(x) for k, x in v.items()}
    if isinstance(v, list):
        return [fix(x) for x in v]
    return v


plistlib.dump(fix(pl), open(os.path.join(kext, "Contents", "Info.plist"), "wb"))
print("defines:", " ".join(defines))
print("Built", kext, "(%d sources)" % len(sources))
