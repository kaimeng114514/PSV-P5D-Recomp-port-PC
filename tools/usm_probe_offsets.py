import io,os,subprocess,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
src=r"C:\dev\p5d_rom\data\vita.cpk"
offs=[0x800,0x33e9a20,0x3b8cc00,0x5a26f20,0x7c6d840,0xb051a40,0xb7f3f20,0x128200e0]
with open(src,"rb") as f:
    data=[]
    for i,o in enumerate(offs):
        end=offs[i+1] if i+1<len(offs) else o+60*1024*1024
        f.seek(o); d=f.read(min(end-o,60*1024*1024))
        p=r"C:\dev\tmp\p5d_movies_extract\probe_%02d.usm"%i
        io.open(p,"wb").write(d)
        data.append((i,hex(o),len(d),p))
for i,off,sz,p in data:
    r=subprocess.run([r"C:\ffmpeg\bin\ffprobe.exe","-hide_banner","-v","error","-select_streams","v:0","-show_entries","stream=width,height,avg_frame_rate,nb_frames:format=duration","-of","csv=p=0",p],capture_output=True)
    print("idx",i,off,"size",sz,"->",r.stdout.decode("utf-8","replace").strip(),"|",r.stderr.decode("utf-8","replace").strip()[:120])
