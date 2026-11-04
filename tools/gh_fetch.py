import io,sys,subprocess,json,urllib.request
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
CURL=r"C:\Windows\System32\curl.exe"

def curl(args, timeout=40):
    try:
        r=subprocess.run([CURL,"-sL","--max-time",str(timeout),"-A","Mozilla/5.0"]+args,capture_output=True,timeout=timeout+5)
        return r.returncode, r.stdout
    except Exception as e:
        return -1, b""

def doh(host):
    try:
        req=urllib.request.Request("https://223.5.5.5/resolve?name=%s&type=A"%host)
        with urllib.request.urlopen(req,timeout=10) as r:
            d=json.loads(r.read().decode("utf-8","replace"))
        for a in d.get("Answer",[]):
            if a.get("type")==1 and a.get("data"): return a["data"]
    except Exception as e:
        print("[ghfetch] DoH fail: %s"%e,file=sys.stderr)
    return None

def fetch(url):
    if "://github.com" in url or "://raw.githubusercontent.com" in url:
        rc,out=curl(["https://gh-proxy.com/"+url])
        if rc==0 and out: return out,"gh-proxy"
        print("[ghfetch] gh-proxy failed rc=%s"%rc,file=sys.stderr)
    rc,out=curl([url])
    if rc==0 and out: return out,"direct"
    print("[ghfetch] direct failed rc=%s"%rc,file=sys.stderr)
    host=url.split("://",1)[1].split("/",1)[0]
    ip=doh(host)
    if ip:
        print("[ghfetch] DoH %s -> %s, retry with --resolve"%(host,ip),file=sys.stderr)
        rc,out=curl(["--resolve","%s:443:%s"%(host,ip),url])
        if rc==0 and out: return out,"doh+resolve"
    return None,None

if __name__=="__main__":
    target=sys.argv[1] if len(sys.argv)>1 else "https://raw.githubusercontent.com/TheLostPantheon/nativ/main/README.md"
    data,how=fetch(target)
    if data is None: print("FETCH_FAILED"); sys.exit(2)
    print("OK via %s bytes=%d"%(how,len(data)))
    sys.stdout.write(data[:400].decode("utf-8","replace"))
