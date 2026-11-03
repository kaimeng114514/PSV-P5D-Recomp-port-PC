import io,os
src=r"C:\dev\p5d_rom\data\vita.cpk"
out=r"C:\dev\tmp\p5d_movies_extract"
os.makedirs(out,exist_ok=True)
with open(src,"rb") as f:
    # first USM at 0x800, size known from prior evidence
    f.seek(0x800); d=f.read(54432288)
    io.open(os.path.join(out,"c_p5d000.usm"),"wb").write(d)
    print("c_p5d000 bytes",len(d),"magic",d[:4], "second block", d[4:8])
    # find next CRID occurrences
    f.seek(0); data=f.read(600*1024*1024)
    offs=[]; i=0
    while True:
        j=data.find(b"CRID",i)
        if j<0: break
        offs.append(j); i=j+4
    print("CRID offsets (first 600MB):",[hex(x) for x in offs[:8]],"count",len(offs))
