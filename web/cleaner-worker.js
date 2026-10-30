/* SPDX-License-Identifier: GPL-3.0-only */
'use strict';
// Hash locally even on LAN HTTP, where Web Crypto is unavailable.
const K = new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
const rotate = (x, n) => (x >>> n) | (x << (32-n));
class SHA256 {
    constructor() { this.h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]); this.buffer = new Uint8Array(64); this.used = 0; this.length = 0; this.w = new Uint32Array(64) }
    block(bytes, at) {
        const w = this.w;
        for (let i=0;i<16;i++) { const j=at+i*4; w[i]=(bytes[j]<<24)|(bytes[j+1]<<16)|(bytes[j+2]<<8)|bytes[j+3] }
        for (let i=16;i<64;i++) { const a=w[i-15], b=w[i-2]; w[i]=(w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>>10)))>>>0 }
        let [a,b,c,d,e,f,g,h]=this.h;
        for(let i=0;i<64;i++) { const t=(h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+K[i]+w[i])>>>0; const u=((rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c)))>>>0; h=g;g=f;f=e;e=(d+t)>>>0;d=c;c=b;b=a;a=(t+u)>>>0 }
        const state=[a,b,c,d,e,f,g,h]; for(let i=0;i<8;i++) this.h[i]=(this.h[i]+state[i])>>>0;
    }
    update(bytes) {
        this.length+=bytes.length; let at=0;
        if(this.used) { const n=Math.min(64-this.used,bytes.length);this.buffer.set(bytes.subarray(0,n),this.used);this.used+=n;at=n;if(this.used===64){this.block(this.buffer,0);this.used=0} }
        while(at+64<=bytes.length){this.block(bytes,at);at+=64}
        if(at<bytes.length){this.buffer.set(bytes.subarray(at),0);this.used=bytes.length-at}
        return this;
    }
    hex() {
        const length=this.length, pad=new Uint8Array(this.used<56?64-this.used:128-this.used);pad[0]=128;
        const v=new DataView(pad.buffer);v.setUint32(pad.length-8,Math.floor(length/0x20000000));v.setUint32(pad.length-4,(length*8)>>>0);this.update(pad);
        return Array.from(this.h,x=>x.toString(16).padStart(8,'0')).join('');
    }
}
const CHUNK=4*1024*1024;
async function hashFile(file) { const hash=new SHA256();for(let at=0;at<file.size;at+=CHUNK)hash.update(new Uint8Array(await file.slice(at,at+CHUNK).arrayBuffer()));return hash.hex() }
const CRC_TABLE=Uint32Array.from({length:256},(_,i)=>{for(let j=0;j<8;j++)i=(i>>>1)^((i&1)?0xedb88320:0);return i>>>0});
function crcUpdate(crc, bytes){for(const b of bytes)crc=(crc>>>8)^CRC_TABLE[(crc^b)&255];return crc>>>0}
function classify(spec, size, hash) {
    if(size===spec.size && hash===spec.sha256)return {kind:'clean',label:'Already clean'};
    const recipe=spec.inputs.find(x=>x.size===size&&x.sha256===hash);
    return recipe?{kind:'old',label:'Ready to unpatch',recipe}:{kind:'unsupported',label:'Unrecognized file — not supported'};
}
function collect(files, manifest) {
    const paths=new Map(), names=new Map();
    for(const spec of manifest.files){const name=spec.path.split('/').pop();if(names.has(name))throw Error('Ambiguous manifest filename');names.set(name,spec.path)}
    for(const file of files){const rel=names.get(file.name);if(!rel)continue;const list=paths.get(rel)||[];list.push(file);paths.set(rel,list)}
    return paths;
}
async function convertFile(file,spec,recipe) {
    const hash=new SHA256(),parts=[];let crc=0xffffffff;
    const records=(recipe?.records||[]).map(([at,hex])=>{
        if(!Number.isSafeInteger(at)||at<0||hex.length%2||!/^[0-9a-f]*$/.test(hex)||at+hex.length/2>spec.size)throw Error('Invalid correction data');
        return [at,Uint8Array.from(hex.match(/../g)||[],s=>parseInt(s,16))];
    });
    for(let at=0;at<spec.size;at+=CHUNK){
        const end=Math.min(at+CHUNK,spec.size),chunk=new Uint8Array(end-at),shift=recipe?.shift||0;
        const from=Math.max(0,at-shift),to=Math.min(file.size,end-shift);
        if(to>from)chunk.set(new Uint8Array(await file.slice(from,to).arrayBuffer()),Math.max(0,shift-at));
        for(const [offset,data] of records){const first=Math.max(at,offset),last=Math.min(end,offset+data.length);if(last>first)chunk.set(data.subarray(first-offset,last-offset),first-at)}
        hash.update(chunk);crc=crcUpdate(crc,chunk);parts.push(chunk);
    }
    if(hash.hex()!==spec.sha256)throw Error('Output verification failed: '+spec.path);
    return {path:spec.path,size:spec.size,crc:(crc^0xffffffff)>>>0,blob:new Blob(parts)};
}
function zip(entries) {
    let offset=0;const parts=[],central=[];const encoder=new TextEncoder();
    for(const entry of entries){
        const name=encoder.encode(entry.path);if(entry.size>0xffffffff||offset>0xffffffff||name.length>65535||entry.path.includes('..')||entry.path.startsWith('/'))throw Error('Invalid ZIP entry');
        const header=new Uint8Array(30+name.length),v=new DataView(header.buffer);v.setUint32(0,0x04034b50,true);v.setUint16(4,20,true);v.setUint16(6,0x800,true);v.setUint16(12,33,true);v.setUint32(14,entry.crc,true);v.setUint32(18,entry.size,true);v.setUint32(22,entry.size,true);v.setUint16(26,name.length,true);header.set(name,30);
        const record=new Uint8Array(46+name.length),c=new DataView(record.buffer);c.setUint32(0,0x02014b50,true);c.setUint16(4,20,true);c.setUint16(6,20,true);c.setUint16(8,0x800,true);c.setUint16(14,33,true);c.setUint32(16,entry.crc,true);c.setUint32(20,entry.size,true);c.setUint32(24,entry.size,true);c.setUint16(28,name.length,true);c.setUint32(42,offset,true);record.set(name,46);central.push(record);parts.push(header,entry.blob);offset+=header.length+entry.size;
    }
    const length=central.reduce((n,x)=>n+x.length,0);if(offset+length>0xffffffff||entries.length>65535)throw Error('ZIP too large');
    const end=new Uint8Array(22),v=new DataView(end.buffer);v.setUint32(0,0x06054b50,true);v.setUint16(8,entries.length,true);v.setUint16(10,entries.length,true);v.setUint32(12,length,true);v.setUint32(16,offset,true);
    return new Blob([...parts,...central,end],{type:'application/zip'});
}
let selection=null, processing=false, inventory=new Map(), activeManifest=null;
async function handle(message, send) {
    if(processing)throw Error('A conversion task is already running');processing=true;
    try{
        if(message.type==='scan'||message.type==='add'){
            const identity=JSON.stringify(message.manifest);
            if(message.type==='scan'){inventory=new Map();activeManifest=identity}
            else if(activeManifest===null)activeManifest=identity;
            else if(activeManifest!==identity)throw Error('Clear files before changing the conversion manifest');
            selection=null;const paths=collect(message.files,message.manifest);
            for(const path of paths.keys())inventory.delete(path);
            for(const spec of message.manifest.files){
                if(message.type==='add'&&!paths.has(spec.path))continue;
                inventory.delete(spec.path);
                let selected=null;
                const found=paths.get(spec.path)||[];let result={kind:'missing',label:spec.required?'Required file missing':spec.unchanged?'Not supplied — no conversion needed':'Optional — not supplied'};
                if(found.length>1)result={kind:'unsupported',label:'Duplicate path — supply one copy of each file'};
                else if(found.length===1){
                    send({type:'progress',text:'Checking '+spec.path});const file=found[0];
                    if(file.size!==spec.size&&!spec.inputs.some(x=>x.size===file.size))result={kind:'unsupported',label:'Unrecognized file size — not supported'};
                    else result=classify(spec,file.size,await hashFile(file));
                    if(result.kind==='old')selected={file,spec,recipe:result.recipe};
                }
                const row={path:spec.path,present:found.length>0,kind:result.kind,label:result.label};
                inventory.set(spec.path,{row,selected});send({type:'row',row});
            }
            const rows=message.manifest.files.map(spec=>inventory.get(spec.path)?.row||{kind:'missing'});
            const selected=message.manifest.files.map(spec=>inventory.get(spec.path)?.selected).filter(Boolean);
            const unsupported=rows.some(row=>row.kind==='unsupported');
            const missingRequired=rows.filter((row,i)=>message.manifest.files[i].required&&row.kind==='missing').length;
            const ready=!unsupported&&selected.length>0;
            if(ready)selection=selected;send({type:'scanned',ready,count:selected.length,missingRequired});
        }else if(message.type==='convert'){
            if(!selection)throw Error('Add at least one supported older patched file first');const entries=[];
            for(const item of selection){send({type:'progress',text:'Unpatching and verifying '+item.spec.path});entries.push(await convertFile(item.file,item.spec,item.recipe))}
            send({type:'complete',blob:zip(entries),count:entries.length});
        }else throw Error('Unknown conversion request');
    }finally{processing=false}
}
if(typeof module!=='undefined')module.exports={SHA256,hashFile,classify,collect,convertFile,zip,handle,crcUpdate};
else self.onmessage=event=>handle(event.data,data=>self.postMessage(data)).catch(error=>self.postMessage({type:'error',text:error.message}));
