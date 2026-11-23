/** Global audio controls, independent of appearance selection and video state. */
import { appearanceStateLabel } from "./lib/appearance-state-label.js";
import React,{useEffect,useId,useLayoutEffect,useRef,useState,useSyncExternalStore} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {Upload,UploadCloud,Trash2,Volume2} from 'lucide-react';
import {compressWavFileForBoard} from './lib/audio-cue.js';
import {defaultAudioCueSrcForFamily} from './lib/builtin-appearances.js';
import {GLOBAL_AUDIO_STATES,globalAudioDirty,globalAudioRevision,confirmGlobalAudio,subscribeGlobalAudio,migrateBuiltinGlobalAudioCues,readGlobalAudioCue,saveGlobalAudioCue,removeGlobalAudioCue} from './lib/global-audio-cues.js';
import {setExitReason,useExitProtection} from './lib/exit-protection.js';
// In the webview invalidate the request before the next committed screen can be used.
const useTargetEffect = typeof window === 'undefined' ? useEffect : useLayoutEffect;
function audioTarget(usb, wifi, boardDeviceId, wifiConnected) {
  if (usb?.connected || (!usb && boardDeviceId && !wifiConnected && !wifi)) {
    const id = usb ? usb.boardDeviceId : boardDeviceId;
    if (id && (boardDeviceId === undefined || id === boardDeviceId)) {
      return {method:'usb', id, session:[usb?.connectionId, usb?.bootId]};
    }
    return {hint:'等待 USB 确认当前设备。'};
  }
  if (wifi?.wifiConnected === true && /^p4-[0-9a-f]{12}$/.test(wifi.boardDeviceId)
      && /^[0-9a-f]{16}$/.test(wifi.bootId)) {
    const target = {id:wifi.boardDeviceId, session:wifi.bootId};
    if (wifi.globalAudioCues === true && wifi.controlTrusted === true) return {...target, method:'wifi'};
    if (wifi.globalAudioCues === true) return {...target, hint:'请先在设备上授权 Wi-Fi 控制，或连接 USB 下发。'};
    return {...target, hint:'当前固件下发全局提示音需要 USB。'};
  }
  return {hint:wifiConnected ? '下发全局提示音需要 USB，或连接支持此功能的受信 Wi-Fi 设备。' : '连接设备后可下发音效。'};
}

export default function GlobalAudioCuesPanel({usb,wifi,boardDeviceId,currentFamily,wifiConnected=false}){
 const [family,setFamily]=useState(()=>GLOBAL_AUDIO_STATES.includes(currentFamily)?currentFamily:'done');
 const [editing,setEditing]=useState(false),[error,setError]=useState(''),[url,setUrl]=useState(''),[custom,setCustom]=useState(false);
 const [transfer,setTransfer]=useState(null), request=useRef(null);
 const revision=useSyncExternalStore(subscribeGlobalAudio,globalAudioRevision,globalAudioRevision);
 const target=audioTarget(usb,wifi,boardDeviceId,wifiConnected);
 const pending=()=>globalAudioDirty(target.id);
 const dirty=useSyncExternalStore(subscribeGlobalAudio,pending,pending);
 const scope=JSON.stringify([target.method,target.id,target.session]);
 useTargetEffect(()=>{
   setTransfer(null);
   return ()=>{request.current=null;};
 },[scope]);
 const sending=transfer?.scope===scope && transfer.busy;
 const busy=editing || sending;
 const syncError=transfer?.scope===scope && transfer.revision===revision && transfer.error;
 const id=useId();useExitProtection(editing?'提示音操作尚未完成':'');
 useEffect(()=>{if(GLOBAL_AUDIO_STATES.includes(currentFamily))setFamily(currentFamily);},[currentFamily]);
 useEffect(()=>{setError('');},[family]);
 useEffect(()=>{let disposed=false,objectUrl='';
  setUrl('');setCustom(false);
  (async()=>{try{if(!window.__TAURI_INTERNALS__){setUrl(defaultAudioCueSrcForFamily(family));return;}
    await migrateBuiltinGlobalAudioCues();const bytes=await readGlobalAudioCue(family);
    if(disposed)return;setCustom(Boolean(bytes));
    objectUrl=bytes?URL.createObjectURL(new Blob([bytes],{type:'audio/wav'})):'';
    setUrl(objectUrl||defaultAudioCueSrcForFamily(family));
   }catch(e){if(!disposed)setError(String(e?.message||e));}})();
  return()=>{disposed=true;if(objectUrl)URL.revokeObjectURL(objectUrl);};
 },[family,revision]);
 async function change(action){if(busy)return;setEditing(true);setError('');
  try{await action();}catch(e){setError(String(e?.message||e));}finally{setEditing(false);}}
 async function upload(event){const file=event.target.files?.[0];event.target.value='';if(!file)return;
  await change(async()=>{if(!/\.wav$/i.test(file.name))throw new Error('请上传 WAV 格式文件');await saveGlobalAudioCue(family,await compressWavFileForBoard(file));});}
 async function sync(){
  if(busy || !target.method || request.current)return;
  const token={};
  request.current=token;
  setExitReason(token,'提示音操作尚未完成');
  setTransfer({scope,busy:true});
  setError('');
  let sentRevision=globalAudioRevision();
  try{
   await migrateBuiltinGlobalAudioCues();
   if(request.current!==token)return;
   sentRevision=globalAudioRevision();
   const result=target.method==='usb'
    ? await invoke('usb_sync_appearance',{appearanceId:'global-audio-cues',boardDeviceId:target.id,audioOnly:true})
    : await invoke('lan_sync_global_audio_cues',{boardDeviceId:target.id,bootId:target.session});
   if(request.current!==token)return;
   if(result?.ok!==true)throw new Error(result?.error||'设备未确认音效下发');
   confirmGlobalAudio(target.id,sentRevision);
   setTransfer(null);
  }catch(e){
   if(request.current===token)setTransfer({scope,revision:sentRevision,error:String(e?.message||e)});
  }finally{
   if(request.current===token)request.current=null;
   setExitReason(token,'');
  }
 }
 return <section className="detail-side-section detail-side-section--audio"><div className="detail-audio-config">
  <div className="detail-audio-config__header"><span><Volume2 size={15}/>全局提示音</span><span className="muted small">所有形象通用</span></div>
  <select className="ui-control ui-control--select" aria-label="提示音状态" value={family} disabled={busy} onChange={e=>setFamily(e.target.value)}>{GLOBAL_AUDIO_STATES.map(f=><option key={f} value={f}>{appearanceStateLabel(f)}</option>)}</select>
  {url?<audio className="detail-audio-config__player" src={url} controls/>:<div className="muted small">此状态未设置提示音</div>}
  <div className="detail-audio-config__actions"><input id={id} type="file" accept=".wav,audio/wav" className="detail-audio-config__input" disabled={busy} onChange={upload}/>
   <label className={`btn-ghost${busy?' is-disabled':''}`} htmlFor={busy?undefined:id}><Upload size={14}/>上传 WAV</label>
   {custom&&<button type="button" className="btn-ghost danger" disabled={busy} onClick={()=>change(()=>removeGlobalAudioCue(family))}><Trash2 size={14}/>恢复默认</button>}
   <button type="button" className="btn-primary" disabled={busy||!target.method} onClick={sync}><UploadCloud size={14}/>{busy?'处理中…':'下发音效'}</button>
   <span className="muted small">{target.hint || (target.method==='wifi'?'通过受信 Wi-Fi 下发。':'通过 USB 下发。')}</span>
  </div>
  {(error||syncError)&&<div role="alert" className="message-banner message-banner--error">{error||syncError}</div>}
  {(target.id||dirty)&&<div role="status" className="message-banner">{dirty
    ? (target.id?'当前设备尚未确认本版提示音。':'提示音已保存，连接设备后可下发。')
    : '当前设备已确认本版全局提示音。'}</div>}
 </div></section>;
}
