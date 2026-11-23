//! Media and note commands remain bound to the route of the visible component.
use super::{component_events::pairing_revision, exchange_link_bounded, query_component_status,
            widget_data::{foreground, Route}, Peer};
use crate::usb_serial::UsbSerialManager;
use serde_json::{json,Value};
use std::time::{Duration,Instant};

const MAX: usize = 16382;

#[derive(Clone)]
pub(crate) struct Binding {
    route: Route,
    pairing:u64,
    pub source: String,
    pub instance: String,
    pub boot: String,
    pub capabilities: std::sync::Arc<Value>,
    wifi: Option<(Peer,u32)>,
}

impl Binding {
    pub fn open(id: &str, usb: &UsbSerialManager) -> Result<Self,String> {
        let pairing=pairing_revision();
        let (target,instance)=crate::component_app::av_view(id)?;
        if !target.local(usb) {return Err("组件连接已变化".into());}
        let (boot,capabilities,wifi)=match &target.route {
            Route::Usb{..} => {
                let (status,live)=usb.autostart_connection().ok_or("USB 连接已变化")?;
                if status.runtime!="esp-p4" || !matches!(&target.route,Route::Usb{board,connection} if *connection==live && board==&status.board_device_id) {
                    return Err("USB 组件连接身份不匹配".into());
                }
                (status.boot_id,status.capabilities,None)
            }
            Route::Wifi{board,boot} => {
                let peer=target.av_peer().ok_or("配对组件连接已失效")?;
                let _lane=foreground()?;
                let status=query_component_status(&peer).map_err(|_|"无线设备未响应能力查询")?;
                let snap=status.component_target.ok_or("固件未提供绑定到本次连接的能力")?;
                if snap.capabilities["widgetComponentAv"]!="paired-component-av-v1" {return Err("此固件未声明配对无线组件媒体能力".into());}
                if &snap.board_device_id!=board || &snap.boot_id!=boot {return Err("无线设备已更换或重启".into());}
                (boot.clone(),snap.capabilities,Some((peer,snap.association)))
            }
        };
        let b=Self{route:target.route,pairing,source:format!("app.{id}"),instance,boot,capabilities:capabilities.into(),wifi};
        if b.is_wifi() {
            let caps=b.request(usb,"caps",json!({})).map_err(|_|"设备未确认此组件的无线媒体和录音能力")?;
            if caps["version"]!=1 || caps["maxBody"]!=MAX || caps["maxCaptureBytes"]!=960000 {
                return Err("无线组件媒体协议或容量不兼容".into());
            }
        }
        Ok(b)
    }
    pub fn route(&self)->&Route {&self.route}
    pub fn board(&self)->&str {match &self.route {Route::Usb{board,..}|Route::Wifi{board,..}=>board}}
    pub fn is_wifi(&self)->bool {self.wifi.is_some()}
    pub fn generation(&self)->u64 {
        match &self.route {Route::Usb{connection,..}=>*connection, Route::Wifi{..}=>self.wifi.as_ref().unwrap().1.into()}
    }
    pub fn local(&self,usb:&UsbSerialManager)->bool {match &self.route {Route::Usb{connection,board}=>{let s=usb.status();s.connected && s.board_device_id==*board && s.boot_id==self.boot && s.connection_id==connection.to_string()},Route::Wifi{..}=>self.pairing==pairing_revision() && super::require_no_usb(usb).is_ok()}}
    pub fn request(&self,usb:&UsbSerialManager,topic:&str,payload:Value)->Result<Value,String> {
        let reply=self.status(usb,topic,payload)?;
        if reply["ok"]!=true {return Err("设备拒绝请求，请检查组件视图、会话、顺序及容量".into());}
        Ok(reply)
    }
    pub fn status(&self,usb:&UsbSerialManager,topic:&str,mut payload:Value)->Result<Value,String> {
        if !self.local(usb) {return Err("组件原连接已断开或更换".into());}
        if let Route::Usb{board,connection}=&self.route {
            return usb.media_status_for_connection(board,*connection,topic,payload);
        }
        let (peer,assoc)=self.wifi.as_ref().ok_or("缺少无线连接")?;
        if topic=="media/begin" {payload["source"]=json!(self.source);}
        let body=frame(self.board(),&self.boot,*assoc,&json!({"source":self.source,"instance":self.instance,"topic":topic,"payload":payload}))?;
        let _lane=foreground()?;
        if !self.local(usb) {return Err("配对或连接已变化".into());}
        let deadline=Duration::from_millis(1000);
        let link=exchange_link_bounded(peer,25,&body,Some(MAX),deadline,Some(Instant::now()+deadline))
            .map_err(|_|"无线组件传输未获回执")?;
        if !self.local(usb) {return Err("无线组件回执已过期".into());}
        let reply:Value=serde_json::from_slice(&link.reply).map_err(|_|"无线组件回执格式无效")?;
        Ok(reply)
    }
    pub fn media(&self,usb:&UsbSerialManager,topic:&str,payload:Value)->Result<Value,String> {
        let first=self.request(usb,topic,payload.clone());
        if first.is_ok() || !self.is_wifi() || topic!="media/chunk" {return first;}
        let seq=payload["seq"].as_u64().ok_or("音频序号无效")?;
        let receipt=self.request(usb,"media/query",json!({"sessionId":payload["sessionId"]}))?;
        if receipt["sessionId"]!=payload["sessionId"] {return Err("媒体会话已更换".into());}
        let next=receipt["nextSeq"].as_u64();
        if seq<u32::MAX as u64 && next==Some(seq+1) {return Ok(receipt);}
        if next!=Some(seq) {return Err("媒体序号不匹配".into());}
        self.request(usb,topic,payload)
    }
}

fn frame(board:&str,boot:&str,association:u32,json:&Value)->Result<Vec<u8>,String>{
    if board.len()!=15 || !board.starts_with("p4-") || !board[3..].bytes().all(|b|b.is_ascii_hexdigit()) ||
        boot.len()!=16 || !boot.bytes().all(|b|b.is_ascii_hexdigit()) || association==0 {return Err("无线设备身份无效".into());}
    let mut body=Vec::from(boot.as_bytes());
    body.extend(board.as_bytes());body.extend(association.to_be_bytes());
    serde_json::to_writer(&mut body,json).map_err(|_|"组件请求无效")?;
    if body.len()>MAX {return Err("组件媒体帧超过容量".into());}
    Ok(body)
}

/// Project only the transport fields consumed by media surfaces.
pub(crate) struct Board {
    pub connected:bool, pub board_device_id:String, pub capabilities:std::sync::Arc<Value>
}
pub fn board_status(usb:&UsbSerialManager,binding:Option<&Binding>)->Board{
    if let Some(b)=binding {return Board{connected:b.local(usb),board_device_id:b.board().into(),capabilities:b.capabilities.clone()};}
    let s=usb.status();Board{connected:s.connected,board_device_id:s.board_device_id,capabilities:s.capabilities.into()}
}

use std::sync::Mutex;
static RADIO:Mutex<Option<(Binding,Instant)>>=Mutex::new(None);

pub fn radio(id:&str,usb:&UsbSerialManager)->Option<Binding>{
    let cached=RADIO.lock().ok()?;
    let (b,seen)=cached.as_ref()?;
    (seen.elapsed()<Duration::from_secs(3) && b.source.strip_prefix("pc.")==Some(id) && b.local(usb)).then(||b.clone())
}
impl Binding {
    pub fn radio_key(&self)->String{
        format!("wifi-{}-{}-{}",self.pairing,self.generation(),self.instance)
    }
}
pub fn radio_worker(usb:UsbSerialManager){
    std::thread::spawn(move||{
        let mut target:Option<(Binding,Instant)>=None;
        let mut visible_before=false;
        loop {
            std::thread::sleep(Duration::from_millis(if visible_before{350}else{1800}));
            if target.as_ref().is_none_or(|(b,seen)|!b.local(&usb)||seen.elapsed()>Duration::from_secs(6)){
                target=None;
                let pairing=pairing_revision();
                let Some(_lane)=super::widget_data::background(&usb)else{continue;};
                let Some(peer)=super::load_peer()else{continue;};
                let Ok(status)=query_component_status(&peer)else{continue;};
                let Some(t)=status.component_target else{continue;};
                if t.capabilities["widgetComponentAv"]!="paired-component-av-v1" {continue;}
                target=Some((Binding{route:Route::Wifi{board:t.board_device_id,boot:t.boot_id.clone()},boot:t.boot_id,
                    pairing,source:String::new(),instance:String::new(),capabilities:t.capabilities.into(),wifi:Some((peer,t.association))},Instant::now()));
            }
            let b=&target.as_ref().unwrap().0;
            let reply=b.request(&usb,"media/poll",json!({}));
            let visible=reply.ok().and_then(|r|{
                let source=r["source"].as_str()?;
                let instance=r["instance"].as_str()?;
                if !source.strip_prefix("pc.").is_some_and(crate::component_extension::valid_id) ||
                    instance.len()!=17 || !instance.bytes().all(|c|c.is_ascii_hexdigit()||c==b'-') || r["event"]["source"]!=source { return None; }
                let mut view=b.clone();view.source=source.into();view.instance=instance.into();
                Some((view,r["event"].clone()))
            });
            visible_before=visible.is_some();
            let mut cache=RADIO.lock().unwrap();
            *cache=visible.as_ref().map(|(v,_)|(v.clone(),Instant::now()));
            drop(cache);
            if let Some((view,event))=visible {crate::component_media::radio_event(&view,event);}
        }
    });
}

#[cfg(test)]
mod tests{
    use super::*;
    use super::super::{aead,confirmation_tests::{accept,assert_no_replay},nonce,session_key};
    use std::io::{Read,Write};
    use std::net::TcpListener;
    const BOOT:&str="0011223344556677";
    const BOARD:&str="p4-001122334455";
    const KEY:[u8;16]=[37;16];

    #[test]
    fn identity_and_complete_body_limit_include_prefix(){
        let mut data=json!({"text":""});
        let overhead=frame(BOARD,BOOT,7,&data).unwrap().len();
        data["text"]=json!("a".repeat(MAX-overhead));
        let packet=frame(BOARD,BOOT,7,&data).unwrap();
        assert_eq!(packet.len(),MAX);
        assert_eq!(&packet[..16],BOOT.as_bytes());assert_eq!(&packet[16..31],BOARD.as_bytes());
        assert_eq!(&packet[31..35],&7u32.to_be_bytes());
        data["text"]=json!("a".repeat(MAX-overhead+1));
        assert!(frame(BOARD,BOOT,7,&data).is_err());
        for (b,r,a) in [("p4-zz1122334455",BOOT,7),(BOARD,"old",7),(BOARD,BOOT,0)]{
            assert!(frame(b,r,a,&json!({})).is_err());
        }
    }

    fn receive(listener:&TcpListener,topic:&str,seq:Option<u64>,reply:Option<Value>){
        let mut stream=accept(listener);
        let mut client=[0;16];stream.read_exact(&mut client).unwrap();
        let server=*uuid::Uuid::new_v4().as_bytes();
        stream.write_all(&server).unwrap();
        let key=session_key(&KEY,&client,&server);
        let mut h=[0;3];stream.read_exact(&mut h).unwrap();assert_eq!(h[0],25);
        let len=u16::from_be_bytes([h[1],h[2]]) as usize;
        let mut cipher=vec![0;len+16];stream.read_exact(&mut cipher).unwrap();
        let p=key.open_in_place(nonce(0),aead::Aad::from(h),&mut cipher).unwrap();
        assert_eq!(&p[..16],BOOT.as_bytes());assert_eq!(&p[16..31],BOARD.as_bytes());assert_eq!(&p[31..35],&7u32.to_be_bytes());
        let obj:Value=serde_json::from_slice(&p[35..]).unwrap();
        assert_eq!(obj["source"],"app.journal");assert_eq!(obj["instance"],"a-1");
        assert_eq!(obj["topic"],topic);assert_eq!(obj["payload"]["seq"].as_u64(),seq);
        if let Some(reply)=reply{
            let mut body=serde_json::to_vec(&reply).unwrap();let n=(body.len() as u16).to_be_bytes();
            key.seal_in_place_append_tag(nonce(1),aead::Aad::from([25,h[1],h[2],n[0],n[1]]),&mut body).unwrap();
            stream.write_all(&n).unwrap();stream.write_all(&body).unwrap();
        }
    }
    fn paired(listener:&TcpListener)->Binding{
        Binding{route:Route::Wifi{board:BOARD.into(),boot:BOOT.into()},pairing:pairing_revision(),
          source:"app.journal".into(),instance:"a-1".into(),boot:BOOT.into(),capabilities:json!({}).into(),
          wifi:Some((Peer{address:listener.local_addr().unwrap(),key:zeroize::Zeroizing::new(KEY)},7))}
    }
    #[test]
    fn missing_chunk_ack_reconciles_only_same_session_and_exact_sequence(){
        for (next,session,success,replay) in [(5,"music-test",true,false),(4,"music-test",true,true),(7,"music-test",false,false),(4,"music-new",false,false)]{
            let listener=TcpListener::bind("127.0.0.1:0").unwrap();let b=paired(&listener);
            let worker=std::thread::spawn(move||{
                receive(&listener,"media/chunk",Some(4),None);
                receive(&listener,"media/query",None,Some(json!({"ok":true,"sessionId":session,"nextSeq":next})));
                if replay{receive(&listener,"media/chunk",Some(4),Some(json!({"ok":true,"sessionId":session,"nextSeq":5})));}
                listener
            });
            let result=b.media(&UsbSerialManager::new(),"media/chunk",json!({"sessionId":"music-test","seq":4,"data":"synthetic"}));
            assert_eq!(result.is_ok(),success);assert_no_replay(worker.join().unwrap());
        }
    }
    #[test]
    fn uncertain_note_start_and_cancelled_pairing_do_not_replay(){
        let listener=TcpListener::bind("127.0.0.1:0").unwrap();let mut b=paired(&listener);
        let worker=std::thread::spawn(move||{receive(&listener,"notes/start",None,None);listener});
        let usb=UsbSerialManager::new();let token=json!({"captureOwner":"lan-note-test"});
        assert!(b.request(&usb,"notes/start",token.clone()).is_err());
        b.pairing=b.pairing.wrapping_sub(1);
        assert!(b.request(&usb,"notes/stop",token).is_err());
        assert_no_replay(worker.join().unwrap());
    }
}
