/* SPDX-License-Identifier: GPL-3.0-only */
(() => {
    'use strict';
    const el=id=>document.getElementById(id), manifest=JSON.parse(el('clean-manifest').textContent);
    manifest.files=manifest.files.filter(spec=>!spec.unchanged).sort((a,b)=>Number(b.required)-Number(a.required));
    const rows=new Map();let worker=null,url=null,files=[],working=false,missingRequired=0;
    const requirement=spec=>spec.required?'Required':spec.unchanged?'Optional · unchanged':'Optional';
    const feedback=text=>{el('clean-feedback').textContent=text;el('clean-feedback').hidden=!text};
    // All supported basenames are unique. Output paths always come from the manifest.
    const byName=new Map(manifest.files.map(spec=>[spec.path.split('/').pop(),spec.path]));
    function pathFor(file){return byName.get(file.name)}
    function setWorking(value){working=value;el('clean-input').disabled=value;el('clean-choose').disabled=value;el('clean-clear').disabled=value||!files.length;el('clean-drop').setAttribute('aria-busy',String(value))}
    for(const spec of manifest.files){
        const row=document.createElement('tr'),mark=document.createElement('td'),name=document.createElement('td'),note=document.createElement('td');
        mark.textContent=spec.required?'X':'—';mark.className=spec.required?'clean-mark clean-missing':'clean-mark';mark.setAttribute('aria-label',spec.required?'Required, not scanned':'Optional, not scanned');
        name.textContent=spec.path;note.textContent=requirement(spec);row.append(mark,name,note);el('clean-files').append(row);rows.set(spec.path,{mark,note,required:spec.required,unchanged:spec.unchanged});
    }
    function reset(){if(worker)worker.terminate();worker=null;missingRequired=0;if(url)URL.revokeObjectURL(url);url=null;el('clean-download').hidden=true;el('clean-download').removeAttribute('href');el('clean-convert').disabled=true;for(const row of rows.values()){row.mark.textContent=row.required?'X':'—';row.note.textContent=requirement(row);row.mark.className=row.required?'clean-mark clean-missing':'clean-mark';row.mark.setAttribute('aria-label',row.required?'Required, not scanned':'Optional, not scanned')}}
    if(!window.Worker||!window.Blob){el('clean-input').disabled=true;el('clean-choose').disabled=true;feedback('Open this page in a desktop browser that supports file selection and Web Workers.');return}
    function addFiles(incoming){
        if(working){feedback('Please wait for verification to finish.');return}
        const relevant=Array.from(incoming).filter(file=>pathFor(file));
        if(!relevant.length){feedback('No matching files found.');return}
        const replacements=new Set(relevant.map(pathFor));
        files=files.filter(file=>!replacements.has(pathFor(file))).concat(relevant);
        if(url)URL.revokeObjectURL(url);url=null;el('clean-download').hidden=true;el('clean-download').removeAttribute('href');el('clean-convert').disabled=true;
        for(const path of replacements){const row=rows.get(path);row.mark.textContent='…';row.mark.className='clean-mark';row.note.textContent='Checking…'}
        if(!worker){
        const script=URL.createObjectURL(new Blob([el('clean-worker').textContent],{type:'text/javascript'}));
        try{worker=new Worker(script)}catch(error){URL.revokeObjectURL(script);feedback('Could not start the converter: '+error.message);return}URL.revokeObjectURL(script);
        worker.onerror=()=>{reset();files=[];setWorking(false);feedback('Converter stopped. Please add your files again.')};
        worker.onmessage=({data})=>{
            if(data.type==='progress')feedback(data.text);
            if(data.type==='row'){const row=rows.get(data.row.path),valid=['clean','old'].includes(data.row.kind);row.mark.textContent=data.row.present?'✓':row.required?'X':'—';row.mark.className='clean-mark '+(valid?'clean-ok':data.row.kind==='unsupported'?'warning':row.required?'clean-missing':'');row.mark.setAttribute('aria-label',data.row.present?'File present':'File missing');row.note.textContent=requirement(row)+' · '+data.row.label}
            if(data.type==='scanned'){missingRequired=data.missingRequired||0;setWorking(false);el('clean-convert').disabled=!data.ready;feedback(data.ready?(missingRequired?`${missingRequired} required file${missingRequired===1?' is':'s are'} missing. You can unpatch the files found.`:'Ready to unpatch.'):'Add a supported older patched file or replace unsupported files.')}
            if(data.type==='complete'){url=URL.createObjectURL(data.blob);el('clean-download').href=url;el('clean-download').hidden=false;setWorking(false);feedback('Clean files ready to download.')}
            if(data.type==='error'){feedback(data.text);setWorking(false);el('clean-convert').disabled=true;el('clean-download').hidden=true}
        };
        }
        setWorking(true);feedback('Checking new files…');worker.postMessage({type:'add',files:relevant,manifest});
    }
    el('clean-choose').onclick=()=>el('clean-input').click();
    el('clean-input').addEventListener('change',()=>{addFiles(el('clean-input').files);el('clean-input').value=''});
    el('clean-clear').onclick=()=>{if(working)return;files=[];reset();setWorking(false);el('clean-input').value='';feedback('')};
    let dragDepth=0;
    for(const name of ['dragenter','dragover','dragleave','drop'])el('clean-drop').addEventListener(name,event=>{
        event.preventDefault();event.stopPropagation();
        if(name==='dragenter')dragDepth++;
        if(name==='dragleave')dragDepth=Math.max(0,dragDepth-1);
        if(name==='dragover'&&event.dataTransfer)event.dataTransfer.dropEffect=working?'none':'copy';
        if(name==='drop'){dragDepth=0;addFiles(event.dataTransfer?.files||[])}
        el('clean-drop').classList.toggle('clean-dragging',dragDepth>0);
    });
    el('clean-convert').onclick=()=>{if(!worker||working)return;if(missingRequired&&!confirm(`${missingRequired} required file${missingRequired===1?' is':'s are'} missing. The ZIP will contain only the files found. Continue?`))return;el('clean-convert').disabled=true;setWorking(true);el('clean-download').hidden=true;feedback('Preparing clean files…');worker.postMessage({type:'convert'})};
    window.addEventListener('pagehide',()=>{if(url)URL.revokeObjectURL(url);if(worker)worker.terminate()});
})();
