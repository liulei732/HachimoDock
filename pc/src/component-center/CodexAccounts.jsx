/** Native account list in the same full-width detail position as StockWatchlist.
 * Credentials remain in the native provider; the renderer receives display metadata only.
 */
import React,{useEffect,useRef,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {ArrowDown,ArrowUp,Plus,RefreshCw} from 'lucide-react';
import {canDisplayEmail,displayChange,moveAccount,loginPhase,accountState} from './codex-accounts-model.js';
import './stock-watchlist.css';
import './codex-accounts.css';

export default function CodexAccounts({component,request=invoke,confirm=message=>window.confirm(message)}) {
  const [data,setData]=useState(null),[error,setError]=useState(''),[pollError,setPollError]=useState(''),[busy,setBusy]=useState(false);
  const mounted=useRef(false),acting=useRef(false),epoch=useRef(0);
  useEffect(()=>{
    mounted.current=true;let stopped=false,timer;
    async function poll(){const version=epoch.current;
      try{if(!acting.current){const reply=await request('codex_accounts_request',{input:{operation:'status'}});
        if(!stopped&&version===epoch.current){setData(reply);setPollError('');}}}
      catch(e){if(!stopped&&version===epoch.current)setPollError(String(e));}
      if(!stopped)timer=setTimeout(poll,2000);
    }
    poll();return()=>{stopped=true;mounted.current=false;++epoch.current;clearTimeout(timer);};
  },[component.id,component.versionHash,request]);
  async function command(input) {
    if(acting.current)return;acting.current=true;const version=++epoch.current;setBusy(true);setError('');
    try{const reply=await request('codex_accounts_request',{input});
      if(mounted.current&&version===epoch.current){setData(reply);setPollError('');}}
    catch(e){if(mounted.current&&version===epoch.current)setError(String(e));}
    finally{if(mounted.current&&version===epoch.current){acting.current=false;setBusy(false);}else acting.current=false;}
  }
  const settings=data?.settings,accounts=data?.accounts||[],login=data?.login;
  const logging=['preparing','pending','cancelling','committing'].includes(login?.phase);
  const locked=busy||logging||!!pollError||!data;
  return <details className="stock-watchlist codex-accounts" open>
    <summary>账号管理 <span className="muted small">{accounts.length}/{data?.maxAccounts||8} 个账号 · {settings?.visible?.length||0} 个显示</span></summary>
    <div className="stock-watchlist__body">
      <p className="muted small">勾选要在设备上显示的账号，用上下箭头调整切换顺序。账号显示与顺序修改会立即保存并生效，无需重新同步组件。添加或重新授权会自动打开 Chrome 登录。</p>
      <div className="stock-watchlist__add">
        <button type="button" className="btn-primary btn-sm" disabled={locked||accounts.length>=data?.maxAccounts} onClick={()=>command({operation:'begin',revision:settings.revision})}><Plus size={14}/>添加账号</button>
        <button type="button" className="btn-ghost btn-sm" disabled={busy||!data||!accounts.some(a=>a.loginSaved)} onClick={()=>command({operation:'refresh'})}><RefreshCw size={14}/>刷新额度</button>
      </div>
      {login&&<div className="codex-accounts__login" role="status"><span>{loginPhase(login)}</span>
        {login.phase==='pending'&&login.canOpen&&<button type="button" className="btn-ghost btn-sm" disabled={busy} onClick={()=>command({operation:'openLogin',loginId:login.id})}>再次打开 Chrome 登录页</button>}
        {login.phase==='pending'&&login.browserError&&<small role="alert">{login.browserError}</small>}
        {['preparing','pending'].includes(login.phase)&&<button type="button" className="btn-ghost btn-sm" disabled={busy} onClick={()=>command({operation:'cancel',loginId:login.id})}>取消登录</button>}
        {logging&&<small>请在浏览器输入密码和验证码；此处无需填写。</small>}
      </div>}
      {(error||pollError)&&<p className="stock-watchlist__notice" role="alert">{error||pollError}</p>}
      {!data&&<p className="muted small">{pollError?'账号管理暂不可用，尚未修改账号。':'读取账号列表…'}</p>}
      {!!accounts.length&&<div className="stock-watchlist__table"><table><thead><tr><th>显示</th><th>账号</th><th>会员等级</th><th>授权状态</th><th>管理</th></tr></thead>
        <tbody>{accounts.map((a,index)=>{
          const supported=canDisplayEmail(component.appRuntime,a.email),shown=settings.visible.includes(a.slot);
          return <tr key={a.slot}>
            <td><input type="checkbox" aria-label={`显示 ${a.email||`账号 ${a.slot}`}`} checked={shown} disabled={locked||(!shown&&(!supported||!a.loginSaved))} onChange={e=>command(displayChange(settings,a.slot,e.target.checked))}/></td>
            <td><strong>{a.email||`账号 ${a.slot} · 未登录`}</strong><small>切换顺序 {index+1}</small>{a.loginSaved&&!supported&&<small className="codex-accounts__warning">设备显示此邮箱前，需要更新额度看板的文字资源。</small>}</td>
            <td><span className={`codex-accounts__plan ${a.plan?.startsWith('Pro')?'codex-accounts__plan--pro':''}`}>{a.plan||'待获取'}</span>
              <small>{a.observedAt?new Date(a.observedAt).toLocaleString():'等待额度查询'}</small></td>
            <td><span>{accountState(a)}</span><small>{a.status&&a.status!==accountState(a)?a.status:''}</small></td>
            <td><div className="stock-watchlist__controls">
              <button type="button" className="icon-btn" aria-label={`上移 ${a.email||a.slot}`} disabled={locked||index===0} onClick={()=>command(moveAccount(settings,a.slot,-1))}><ArrowUp size={14}/></button>
              <button type="button" className="icon-btn" aria-label={`下移 ${a.email||a.slot}`} disabled={locked||index===accounts.length-1} onClick={()=>command(moveAccount(settings,a.slot,1))}><ArrowDown size={14}/></button>
              <button type="button" className="btn-ghost btn-sm" disabled={locked} onClick={()=>command({operation:'begin',revision:settings.revision,slot:a.slot})}>{a.loginSaved?'重新授权':'登录账号'}</button>
              <button type="button" className="btn-ghost btn-sm" disabled={locked} onClick={()=>{if(confirm(`从额度看板移除 ${a.email||`账号 ${a.slot}`}？该账号不再显示，本机授权将保留在恢复备份中；不影响当前聊天登录。`))command({operation:'remove',revision:settings.revision,slot:a.slot,confirm:true});}}>移除</button>
            </div></td>
          </tr>;
        })}</tbody></table></div>}
      {data&&!accounts.length&&<p className="muted small">还没有账号，点击“添加账号”登录。</p>}
      {data&&!settings.visible.length&&<p className="stock-watchlist__notice">未勾选显示账号；设备将显示等待状态，不沿用已移除账号的额度。</p>}
      <p className="muted small">会员等级取自对应账号的额度查询，查询失败时保留上次结果并显示更新时间。授权仅保存在这台电脑，不进入组件包和设备。</p>
    </div>
  </details>;
}
