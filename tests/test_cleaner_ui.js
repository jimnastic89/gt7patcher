// Exercise the generated release page, not just the editable source template.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../build/index.html'),'utf8');
assert(html.includes('id="live-tab"'));assert(html.includes('id="live" role='));
const elements=new Map();
function element(){return {value:'',checked:false,disabled:false,hidden:false,textContent:'',dataset:{},children:[],listeners:{},attributes:{},classList:{toggle(){},add(){},remove(){}},append(...items){this.children.push(...items)},setAttribute(k,v){this.attributes[k]=v},removeAttribute(k){delete this[k];delete this.attributes[k]},querySelectorAll(){return []},addEventListener(k,fn){this.listeners[k]=fn}}}
for(const match of html.matchAll(/<[a-z]+\b([^>]*\bid="([^"]+)"[^>]*)>/g)){const e=element();e.hidden=/\bhidden\b/.test(match[1]);e.disabled=/\bdisabled\b/.test(match[1]);elements.set(match[2],e)}
for(const id of ['clean-manifest','clean-worker'])elements.get(id).textContent=html.match(new RegExp('<script id="'+id+'"[^>]*>([\\s\\S]*?)</script>'))[1];
assert(!html.includes('webkitdirectory'));assert.match(html,/id="clean-input"[^>]*accept="\.bin,\.prx,\.sprx,\.index,\.psp"/);
let worker,revoked=[],created=0,requests=[];
class Worker {constructor(){worker=this;this.messages=[]}postMessage(m){this.messages.push(m)}terminate(){this.terminated=true}}
const state={token:'test',version:'0.3',config:{mask:0,preset:0,units:0,language:-1,speed:0,mass:0,power:0,torque:0,temperature:1,autostart:0},features:[],gameRunning:0,engineRunning:false,message:'Ready'};
const context=vm.createContext({document:{activeElement:null,getElementById:id=>elements.get(id)||null,createElement:element},window:{Worker,Blob,addEventListener(){}},Worker,Blob,URL:{createObjectURL(){return 'blob:test-'+(++created)},revokeObjectURL(x){revoked.push(x)}},Option:function(){},URLSearchParams,confirm:()=>true,clearTimeout(){},setTimeout(){},setInterval(){},fetch:async(path,options={})=>{requests.push({path,options});return {ok:true,json:async()=>state}}});
const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];assert.equal(scripts.length,2);
vm.runInContext(scripts[0][1],context);vm.runInContext(scripts[1][1],context);
const get=id=>elements.get(id),settle=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
    assert.match(html,/Select patches, then start the engine before launching GT7\./);
    await settle();
    assert.match(html,/\$\('start'\)\.onclick = async \(\) => \{[\s\S]*?if \(dirty\) \{[\s\S]*?fetch\('\/api\/config'[\s\S]*?fetch\('\/api\/start'/);
    get('help-tab').onclick();assert.equal(get('help').hidden,false);assert.equal(get('controls').hidden,true);
    assert.equal(get('clean-files').children.length,11);
    assert.equal(get('clean-files').children.filter(row=>row.children[0].textContent==='X').length,7);
    assert(get('clean-files').children.slice(0,7).every(row=>row.children[2].textContent==='Required'));
    assert(get('clean-files').children.slice(7).every(row=>row.children[2].textContent==='Optional'));
    assert.equal(get('clean-files').children.filter(row=>row.children[2].textContent==='Optional · unchanged').length,0);
    assert.match(get('clean-files').children[0].children[0].className,/clean-mark/);assert.match(html,/\.clean-missing\s*\{[^}]*color:#ff7373/);assert.match(html,/\.clean-mark\s*\{[^}]*font-weight:800/);
    const listed=get('clean-files').children.map(row=>row.children[1].textContent);for(const name of ['ampr_emu.index','fakelib/libSceAmpr.sprx','fakelib/libScePlayGo.sprx'])assert(!listed.includes(name));
    assert.equal(get('clean-download').hidden,true);assert.equal(get('clean-convert').disabled,true);
    assert.equal(get('clean-feedback').hidden,true);assert(!html.includes('Add files to begin.'));assert(!html.includes('Back up your game files, then extract the ZIP'));
    let pickerOpened=0;get('clean-input').click=()=>pickerOpened++;get('clean-choose').onclick();assert.equal(pickerOpened,1);assert.equal(get('clean-input').hidden,true);
    get('clean-input').files=[{name:'eboot.bin',webkitRelativePath:'dump/eboot.bin'}];get('clean-input').listeners.change();assert.equal(worker.messages[0].type,'add');assert.equal(get('clean-choose').disabled,true);assert.equal(get('clean-feedback').hidden,false);
    worker.onmessage({data:{type:'row',row:{path:'eboot.bin',present:true,kind:'old',label:'Ready to unpatch'}}});
    const eboot=get('clean-files').children.find(row=>row.children[1].textContent==='eboot.bin');assert.equal(eboot.children[0].textContent,'✓');
    worker.onmessage({data:{type:'scanned',ready:true,count:1,missingRequired:6}});assert.equal(get('clean-convert').disabled,false);assert.match(get('clean-feedback').textContent,/6 required files are missing/);
    worker.onmessage({data:{type:'scanned',ready:true,count:7,missingRequired:0}});assert.equal(get('clean-convert').disabled,false);
    get('clean-convert').onclick();assert.equal(worker.messages.at(-1).type,'convert');assert.equal(get('clean-download').hidden,true);
    worker.onmessage({data:{type:'complete',count:7,blob:new Blob(['zip'])}});assert.equal(get('clean-download').hidden,false);assert.match(get('clean-download').href,/^blob:/);
    const previous=worker;get('clean-input').files=[{name:'libc.prx'}];get('clean-input').listeners.change();assert.equal(worker,previous);assert.equal(worker.messages.at(-1).files.length,1);assert.equal(worker.messages.at(-1).files[0].name,'libc.prx');assert.equal(eboot.children[0].textContent,'✓');assert(!previous.terminated);assert.equal(get('clean-download').hidden,true);assert.equal(get('clean-convert').disabled,true);assert(revoked.length>=2);
    worker.onmessage({data:{type:'error',text:'Bad input'}});assert.equal(get('clean-input').disabled,false);assert.equal(get('clean-download').hidden,true);
    let prevented=false;
    get('clean-drop').listeners.dragenter({preventDefault(){prevented=true},stopPropagation(){}});assert(prevented);
    const dropped={name:'libScePfs.prx'};
    get('clean-drop').listeners.drop({preventDefault(){},stopPropagation(){},dataTransfer:{files:[dropped,{name:'unrelated.txt'}]}});
    assert.equal(worker.messages.at(-1).files.length,1);assert(worker.messages.at(-1).files.includes(dropped));assert(!worker.messages.at(-1).files.some(file=>file.name==='unrelated.txt'));
    const active=worker;get('clean-drop').listeners.drop({preventDefault(){},stopPropagation(){},dataTransfer:{files:[{name:'logiWheel.prx'}]}});assert.equal(worker,active);
    worker.onmessage({data:{type:'scanned',ready:false,count:3}});
    get('clean-clear').onclick();assert.equal(get('clean-feedback').hidden,true);assert.equal(get('clean-choose').disabled,false);assert.equal(get('clean-clear').disabled,true);assert.equal(get('clean-convert').disabled,true);assert.equal(get('clean-download').hidden,true);
    get('clean-drop').listeners.drop({preventDefault(){},stopPropagation(){},dataTransfer:{files:[{name:'unrelated.txt'}]}});assert.equal(get('clean-convert').disabled,true);
    console.log('PASS: generated UI boots with Live Settings; Help navigation, 11-row checklist, scan gate, ticks, conversion, ZIP link, rescan reset, incremental worker reuse, preserved ticks, bold/red indicators, batch addition, drop events, unrelated-file rejection, busy guard, clear and errors');
})().catch(error=>{console.error(error);process.exitCode=1});
