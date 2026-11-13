from __future__ import annotations
import glob, ipaddress, os, re, secrets, socket, threading, time, uuid
from pathlib import Path
from urllib.parse import urlparse
from flask import Flask, jsonify, request, send_file
from yt_dlp import YoutubeDL
from yt_dlp.utils import DownloadError

ROOT=Path(__file__).resolve().parent
DATA=Path(os.environ.get("VIDEODL_DATA",str(ROOT/"data"))).expanduser()
DOWNLOADS=Path(os.environ.get("VIDEODL_DOWNLOADS",str(Path.home()/"storage"/"downloads"/"VideoDL"))).expanduser()
DATA.mkdir(parents=True,exist_ok=True); DOWNLOADS.mkdir(parents=True,exist_ok=True)
KEY_FILE=DATA/"access.key"
if not KEY_FILE.exists():
    KEY_FILE.write_text(secrets.token_urlsafe(32),encoding="utf-8")
    try: KEY_FILE.chmod(0o600)
    except OSError: pass
ACCESS_KEY=KEY_FILE.read_text(encoding="utf-8").strip()
app=Flask(__name__,static_folder=str(ROOT/"static"),static_url_path="/static")
app.config["MAX_CONTENT_LENGTH"]=16*1024
TASKS={}; LOCK=threading.RLock(); ACTIVE=0
LIMIT=max(1,min(int(os.environ.get("VIDEODL_MAX_TASKS","2")),3))

def valid_url(value):
    if not isinstance(value,str) or len(value)>4096: raise ValueError("链接无效或过长")
    p=urlparse(value.strip())
    if p.scheme not in ("http","https") or not p.hostname: raise ValueError("只支持 http/https 视频链接")
    host=p.hostname.rstrip(".").lower()
    if host=="localhost" or host.endswith((".localhost",".local")): raise ValueError("不允许访问本机或局域网地址")
    try: ips={ipaddress.ip_address(host)}
    except ValueError:
        try: ips={ipaddress.ip_address(x[4][0]) for x in socket.getaddrinfo(host,None)}
        except (OSError,ValueError): raise ValueError("无法解析视频站点域名")
    if not ips or any(not x.is_global for x in ips): raise ValueError("不允许访问内网或保留地址")
    return value.strip()

def auth():
    if not secrets.compare_digest(request.headers.get("X-Access-Key",""),ACCESS_KEY):
        return jsonify(ok=False,error="访问密钥不正确"),401
    return None

def opts(extra=None):
    d={"quiet":True,"no_warnings":True,"noplaylist":True,"socket_timeout":20,"retries":2,"extractor_retries":1}
    if extra: d.update(extra)
    return d

def clean(info):
    fs=[]; seen=set()
    for f in info.get("formats") or []:
        if not f.get("url") or f.get("vcodec")=="none": continue
        fid=str(f.get("format_id",""))
        if not fid or fid in seen: continue
        seen.add(fid); h=f.get("height"); fps=f.get("fps")
        fs.append({"format_id":(fid if f.get("acodec") not in (None,"none") else fid+"+bestaudio/best"),"height":h,"fps":fps,"ext":f.get("ext") or "mp4",
          "filesize":f.get("filesize") or f.get("filesize_approx"),
          "has_audio":f.get("acodec") not in (None,"none"),
          "label":(str(h)+"p" if h else "视频流")+((" · "+str(round(fps))+" FPS") if fps else "")})
    fs.sort(key=lambda x:(x["height"] or 0,x["fps"] or 0,x["has_audio"]),reverse=True)
    return {"id":str(info.get("id","")),"title":str(info.get("title") or "未命名视频")[:300],
      "thumbnail":info.get("thumbnail"),"duration":info.get("duration"),
      "uploader":info.get("uploader") or info.get("channel"),"formats":fs[:100]}

@app.get("/")
def home(): return app.send_static_file("index.html")
@app.get("/api/health")
def health(): return jsonify(ok=True,service="VideoDL-Termux",active=sum(1 for x in TASKS.values() if x["status"]=="downloading"))
@app.get("/api/auth/check")
def check():
    e=auth()
    return e if e else jsonify(ok=True)

@app.post("/api/extract")
def extract():
    e=auth()
    if e: return e
    try:
        url=valid_url((request.get_json(silent=True) or {}).get("url",""))
        with YoutubeDL(opts()) as y: info=y.extract_info(url,download=False)
        if info.get("_type")=="playlist": return jsonify(ok=False,error="暂不支持播放列表，请输入单个视频链接"),422
        return jsonify(ok=True,video=clean(info))
    except (ValueError,DownloadError) as ex: return jsonify(ok=False,error=str(ex)[:500]),422
    except Exception:
        app.logger.exception("extract failed")
        return jsonify(ok=False,error="解析失败，请检查链接或稍后重试"),500

def worker(tid,url,fid):
    global ACTIVE
    def hook(d):
        with LOCK:
            t=TASKS.get(tid)
            if not t: return
            if d.get("status")=="downloading":
                total=d.get("total_bytes") or d.get("total_bytes_estimate"); done=d.get("downloaded_bytes",0)
                t.update(progress=round(min(99.9,done*100/total),1) if total else None,downloaded=done,total=total,speed=d.get("speed"),eta=d.get("eta"),phase="正在下载")
            elif d.get("status")=="finished": t.update(progress=99.5,phase="正在合并音视频 / 封装文件")
    try:
        out=str(DOWNLOADS/(tid+"_%(title).120B.%(ext)s"))
        with YoutubeDL(opts({"format":fid,"outtmpl":out,"merge_output_format":"mp4","progress_hooks":[hook],"continuedl":True,"overwrites":False,"writethumbnail":False,"writeinfojson":False})) as y:
            info=y.extract_info(url,download=True); prepared=y.prepare_filename(info)
        files=[Path(prepared)]+[Path(x) for x in glob.glob(str(DOWNLOADS/(tid+"_*")))]
        result=next((p for p in files if p.is_file() and p.suffix not in (".part",".ytdl")),None)
        if result is None: raise RuntimeError("下载结束，但没有找到输出文件")
        with LOCK: TASKS[tid].update(status="completed",progress=100,phase="下载完成",file=str(result),filename=result.name)
    except Exception as ex:
        with LOCK:
            if tid in TASKS: TASKS[tid].update(status="failed",phase="下载失败",error=str(ex)[:500])
    finally:
        with LOCK: ACTIVE=max(0,ACTIVE-1)

@app.post("/api/downloads")
def create():
    global ACTIVE
    e=auth()
    if e: return e
    body=request.get_json(silent=True) or {}
    try:
        url=valid_url(body.get("url","")); fid=str(body.get("format_id","best"))
        if not re.fullmatch(r"[A-Za-z0-9._+/-]{1,100}",fid): raise ValueError("格式编号无效")
    except ValueError as ex: return jsonify(ok=False,error=str(ex)),400
    with LOCK:
        if ACTIVE>=LIMIT: return jsonify(ok=False,error="当前下载任务已满，请稍后再试"),429
        tid=uuid.uuid4().hex[:12]
        TASKS[tid]={"id":tid,"status":"downloading","progress":0,"phase":"正在准备下载","created":int(time.time()),"downloaded":0,"total":None,"speed":None,"eta":None}
        ACTIVE+=1
    threading.Thread(target=worker,args=(tid,url,fid),daemon=True).start()
    return jsonify(ok=True,task_id=tid),202

@app.get("/api/tasks")
def tasks():
    e=auth()
    if e: return e
    with LOCK: items=[{k:v for k,v in t.items() if k!="file"} for t in TASKS.values()]
    items.sort(key=lambda x:x["created"],reverse=True)
    return jsonify(ok=True,tasks=items[:100])
@app.get("/api/files/<tid>")
def file(tid):
    e=auth()
    if e: return e
    with LOCK: t=TASKS.get(tid); p=Path(t["file"]) if t and t.get("file") else None
    if not p or not p.is_file(): return jsonify(ok=False,error="文件尚未准备好"),404
    return send_file(p,as_attachment=True,download_name=t.get("filename",p.name),conditional=True)

if __name__=="__main__":
    from waitress import serve
    print("VideoDL listening on http://127.0.0.1:8765")
    print("Access key: "+ACCESS_KEY)
    serve(app,host="127.0.0.1",port=8765,threads=8)
