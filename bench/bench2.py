#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# v5 加固版：连接被 RST 时记为失败并继续，而不是让整个压测崩溃
import socket, threading, time, json, statistics, sys, os

HOST='127.0.0.1'; PORT=int(os.environ.get('BENCH_PORT','8080'))
N_SEQ=int(os.environ.get('BENCH_SEQ','300'))
N_C1=int(os.environ.get('BENCH_C1','100')); N_P1=int(os.environ.get('BENCH_P1','1'))
N_C3=int(os.environ.get('BENCH_C3','32'));  N_P3=int(os.environ.get('BENCH_P3','25'))
PATH=os.environ.get('BENCH_PATH','/DefaultPage.html')

def log(msg):
    print(msg, flush=True)

def connect():
    s=socket.create_connection((HOST,PORT),timeout=15)
    s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
    return s

def recv_one_response(s,buf):
    try:
        while b"\r\n\r\n" not in buf:
            d=s.recv(4096)
            if not d: return None
            buf+=d
        header,_,rest=buf.partition(b"\r\n\r\n")
        clen=0
        for line in header.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen=int(line.split(b":")[1].strip())
        while len(rest)<clen:
            d=s.recv(4096)
            if not d: return None
            rest+=d
        ok = header.startswith(b"HTTP/1.1 200") or header.startswith(b"HTTP/1.0 200")
        return ok, rest[clen:]
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
        return None   # 连接被服务器 RST：按失败计，不抛异常

def req_close():
    t0=time.perf_counter()
    s=connect()
    try:
        s.sendall(("GET %s HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n"%PATH).encode())
        r=recv_one_response(s,b"")
        if r is None: return False,(time.perf_counter()-t0)*1000
        ok,_=r
    finally:
        try: s.close()
        except Exception: pass
    return ok,(time.perf_counter()-t0)*1000

class KeepAlive:
    def __init__(self):
        self.s=connect(); self.buf=b""
    def request(self):
        t0=time.perf_counter()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n"%PATH).encode())
        r=recv_one_response(self.s,self.buf)
        if r is None: raise ConnectionError("closed/reset by server")
        ok,leftover=r
        self.buf=leftover
        return ok,(time.perf_counter()-t0)*1000
    def close(self):
        try: self.s.close()
        except Exception: pass

def pct(sl,q):
    return sl[max(0,int(len(sl)*q)-1)]

def progress(cur,total):
    if cur % max(1,total//10) == 0 or cur==total:
        bar='#'*(cur*20//total)
        sys.stdout.write("\r  [%-20s] %d/%d"%(bar,cur,total))
        sys.stdout.flush()
        if cur==total: print(" 完成",flush=True)

def run_load(mode,conns,per):
    lat=[]; lock=threading.Lock(); fails=[0]; errs=[0]
    done=[0]
    def worker():
        my=[]; myfail=0; myerr=0
        if mode=='keep':
            ka=None
            for i in range(per):
                if ka is None:
                    try: ka=KeepAlive()
                    except Exception: myerr+=1; continue
                try:
                    o,l=ka.request()
                    my.append(l)
                    if not o: myfail+=1
                except Exception:
                    ka.close(); ka=None; myerr+=1   # 连接被重置：重建连接继续
            if ka: ka.close()
        else:
            for i in range(per):
                o,l=req_close()
                my.append(l)
                if not o: myfail+=1
        with lock:
            lat.extend(my); fails[0]+=myfail; errs[0]+=myerr
            done[0]+=1
            progress(done[0],conns)
    t0=time.perf_counter()
    ts=[threading.Thread(target=worker) for _ in range(conns)]
    for t in ts: t.start()
    for t in ts: t.join()
    el=time.perf_counter()-t0
    return lat,el,fails[0],errs[0]

def main():
    mode_load=sys.argv[1] if len(sys.argv)>1 else 'close'
    out={'path':PATH,'mode':mode_load,'scale':{'seq':N_SEQ,'conc':[N_C1,N_P1],'load':[N_C3,N_P3]}}
    log("压测目标 %s:%d%s  模式=%s"%(HOST,PORT,PATH,mode_load))
    log("[预热] 10 次请求...")
    for _ in range(50):
        try: req_close()
        except Exception: time.sleep(0.2)

    log("[阶段1/3] 顺序短连接 %d 次（每次新建连接）"%N_SEQ)
    ok=0; lat=[]
    for i in range(N_SEQ):
        o,l=req_close(); ok+=o; lat.append(l)
        progress(i+1,N_SEQ)
    s=sorted(lat)
    out['seq_short']={'desc':'sequential, one connection per request','n':N_SEQ,'ok':ok,'qps':round(N_SEQ/(sum(lat)/1000.0),1),'avg_ms':round(statistics.mean(s),3),'p50_ms':round(pct(s,0.5),3),'p99_ms':round(pct(s,0.99),3)}
    log("  QPS=%.1f  avg=%.3fms  p50=%.3fms  p99=%.3fms  失败=%d"%(out['seq_short']['qps'],out['seq_short']['avg_ms'],out['seq_short']['p50_ms'],out['seq_short']['p99_ms'],N_SEQ-ok))

    log("[阶段2/3] 突发并发：%d 连接 x %d 请求"%(N_C1,N_P1))
    lat2,el2,f2,e2=run_load('close',N_C1,N_P1)
    s2=sorted(lat2)
    out['conc_short']={'desc':'concurrent connections x requests','conns':N_C1,'per':N_P1,'total':len(s2),'qps':round(len(s2)/el2,1),'avg_ms':round(statistics.mean(s2),3),'p99_ms':round(pct(s2,0.99),3),'http_fail':f2,'client_error':e2}
    log("  QPS=%.1f  avg=%.3fms  p99=%.3fms  失败=%d"%(out['conc_short']['qps'],out['conc_short']['avg_ms'],out['conc_short']['p99_ms'],f2+e2))

    log("[阶段3/3] 混合负载：%d 并发 x %d 请求（mode=%s）"%(N_C3,N_P3,mode_load))
    lat3,el3,f3,e3=run_load(mode_load,N_C3,N_P3)
    s3=sorted(lat3)
    out['load']={'desc':'concurrent connections x requests','mode':mode_load,'conns':N_C3,'per':N_P3,'total':len(s3),'elapsed_s':round(el3,3),'qps':round(len(s3)/el3,1),'avg_ms':round(statistics.mean(s3),3),'p50_ms':round(pct(s3,0.5),3),'p99_ms':round(pct(s3,0.99),3),'http_fail':f3,'client_error':e3}
    log("  QPS=%.1f  avg=%.3fms  p50=%.3fms  p99=%.3fms  失败=%d"%(out['load']['qps'],out['load']['avg_ms'],out['load']['p50_ms'],out['load']['p99_ms'],f3+e3))

    print("\n===== 最终 JSON 结果 =====",flush=True)
    print(json.dumps(out,indent=1,ensure_ascii=False),flush=True)

if __name__=='__main__':
    main()
