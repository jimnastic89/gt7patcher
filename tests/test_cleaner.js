/* Run with node tests/test_cleaner.js [GT7 workspace] to verify real fixtures too. */
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {SHA256,hashFile,classify,collect,convertFile,zip,handle,crcUpdate}=require('../web/cleaner-worker.js');
const digest=b=>crypto.createHash('sha256').update(b).digest('hex');
function file(bytes,rel){const blob=new Blob([bytes]);blob.name=path.basename(rel);blob.webkitRelativePath='dump/'+rel;return blob}
(async()=>{
    for(const size of [0,1,55,56,63,64,65,127,1000000]){const bytes=crypto.randomBytes(size),hash=new SHA256();for(let at=0;at<size;at+=17)hash.update(bytes.subarray(at,at+17));assert.equal(hash.hex(),digest(bytes));assert.equal(await hashFile(new Blob([bytes])),digest(bytes))}
    assert.equal((crcUpdate(0xffffffff,Buffer.from('123456789'))^0xffffffff)>>>0,0xcbf43926);
    const clean=Buffer.from('clean data'),old=Buffer.from('old'),spec={path:'eboot.bin',required:true,size:clean.length,sha256:digest(clean),inputs:[{size:3,sha256:digest(old),shift:48,records:[[0,clean.toString('hex')]]}]};
    assert.equal(classify(spec,3,digest(old)).kind,'old');assert.equal(classify(spec,clean.length,digest(clean)).kind,'clean');assert.equal(classify(spec,3,digest('bad')).kind,'unsupported');
    const optional={path:'fakelib/extra.sprx',required:false,size:1,sha256:digest('x'),inputs:[]},manifest={files:[spec,optional]},events=[];
    const send=message=>events.push(message);
    await assert.rejects(handle({type:'convert'},send),/Add at least/);
    await handle({type:'scan',manifest,files:[file(old,'eboot.bin')]},send);assert.equal(events.at(-1).ready,true);
    await handle({type:'convert'},send);assert.equal(events.at(-1).type,'complete');
    await handle({type:'scan',manifest,files:[]},send);assert.equal(events.at(-1).ready,false);await assert.rejects(handle({type:'convert'},send),/Add at least/);
    await handle({type:'scan',manifest,files:[file(old,'eboot.bin'),file(old,'eboot.bin')]},send);assert.equal(events.at(-1).ready,false);
    await handle({type:'scan',manifest,files:[file(Buffer.from('bad'),'eboot.bin')]},send);assert.equal(events.at(-1).ready,false);
    const broken=structuredClone(spec.inputs[0]);broken.records[0][1]='00';await assert.rejects(convertFile(file(old,'eboot.bin'),spec,broken),/verification failed/);
    const boundary=crypto.randomBytes(4*1024*1024+123),target=Buffer.concat([Buffer.alloc(48),boundary,Buffer.alloc(8)]);target.fill(42,4*1024*1024-5,4*1024*1024+5);
    const bs={path:'nested/file.bin',size:target.length,sha256:digest(target)},br={shift:48,records:[[4*1024*1024-5,Buffer.alloc(10,42).toString('hex')]]};assert.equal(digest(Buffer.from(await (await convertFile(new Blob([boundary]),bs,br)).blob.arrayBuffer())),digest(target));
    const nested={path:'sce_module/libc.prx',required:true,size:clean.length,sha256:digest(clean),inputs:[]};
    const flat=file(clean,'libc.prx');flat.webkitRelativePath='';const collected=collect([flat,file(old,'unrelated.bin')],{files:[nested]});assert.equal(collected.get('sce_module/libc.prx')[0],flat);assert.equal(collected.size,1);
    await handle({type:'scan',manifest:{files:[nested]},files:[flat]},send);assert.equal(events.at(-1).ready,false);
    await assert.rejects(handle({type:'convert'},send),/Add at least/);
    // Keep the same worker alive and prove old files are never re-read on add.
    let firstReads=0,secondReads=0;
    const firstBlob=new Blob([old]),secondBlob=new Blob([clean]);
    const first={name:'eboot.bin',size:old.length,slice(...args){firstReads++;return firstBlob.slice(...args)}};
    const second={name:'libc.prx',size:clean.length,slice(...args){secondReads++;return secondBlob.slice(...args)}};
    const incremental={files:[spec,nested]};
    await handle({type:'scan',manifest:incremental,files:[]},send);
    await handle({type:'add',manifest:incremental,files:[first]},send);assert.equal(events.at(-1).ready,true);assert.equal(events.at(-1).missingRequired,1);const verifiedReads=firstReads;assert(verifiedReads>0);
    await handle({type:'add',manifest:incremental,files:[second]},send);assert.equal(events.at(-1).ready,true);assert.equal(events.at(-1).count,1);assert.equal(firstReads,verifiedReads);const retainedSecond=secondReads;
    await handle({type:'add',manifest:incremental,files:[file(Buffer.from('bad'),'eboot.bin')]},send);assert.equal(events.at(-1).ready,false);assert.equal(secondReads,retainedSecond);
    await handle({type:'add',manifest:incremental,files:[first]},send);assert.equal(events.at(-1).ready,true);assert(firstReads>verifiedReads);assert.equal(secondReads,retainedSecond);
    await handle({type:'convert'},send);assert.equal(events.at(-1).count,1);
    await handle({type:'scan',manifest:incremental,files:[]},send);assert.equal(events.at(-1).ready,false);await assert.rejects(handle({type:'convert'},send),/Add at least/);
    console.log('PASS: incremental verification reads only additions/replacements; cached readiness, rejection, conversion and clear');
    console.log('PASS: SHA-256 boundaries, CRC32, scan guards, duplicates, stale scan rejection, corrupt correction rejection, chunk-crossing reconstruction');
    const root=process.argv[2];if(root){
        const model=JSON.parse(fs.readFileSync(path.join(__dirname,'../data/cleaner.json'))),entries=[];
        for(const s of model.files){const cleanBytes=fs.readFileSync(path.join(root,'GRAN_TURISMO_7_Clean_Backport',s.path));assert.equal(digest(cleanBytes),s.sha256);const oldPath=path.join(root,'PPSA01317-app',s.path),input=fs.existsSync(oldPath)?fs.readFileSync(oldPath):cleanBytes;const state=classify(s,input.length,digest(input));assert.notEqual(state.kind,'unsupported');const converted=await convertFile(new Blob([input]),s,state.recipe);assert.equal(digest(Buffer.from(await converted.blob.arrayBuffer())),s.sha256);entries.push(converted);console.log('VERIFIED',s.path)}
        fs.writeFileSync(path.join(__dirname,'../build/cleaner-test.zip'),Buffer.from(await zip(entries).arrayBuffer()));
        // Test the seven-file converter input with all unchanged and additional files omitted.
        const supplied=model.files.filter(s=>s.required).map(s=>file(fs.readFileSync(path.join(root,'PPSA01317-app',s.path)),s.path));
        await handle({type:'scan',manifest:model,files:supplied},send);assert.equal(events.at(-1).ready,true);assert.equal(events.at(-1).count,7);
        const missing=supplied.slice(1);await handle({type:'scan',manifest:model,files:missing},send);assert.equal(events.at(-1).ready,true);assert.equal(events.at(-1).missingRequired,1);await handle({type:'convert'},send);assert.equal(events.at(-1).count,6);
        await handle({type:'scan',manifest:model,files:supplied},send);assert.equal(events.at(-1).ready,true);
        await handle({type:'convert'},send);assert.equal(events.at(-1).count,7);fs.writeFileSync(path.join(__dirname,'../build/cleaner-required-test.zip'),Buffer.from(await events.at(-1).blob.arrayBuffer()));
        console.log('PASS: all 14 real reference outputs and required-only scan/conversion ZIP');
    }
})().catch(error=>{console.error(error);process.exitCode=1});
