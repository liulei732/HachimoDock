// Actual settings panels and DOM keyboard/focus events. Only Tauri IPC is simulated.
import assert from "node:assert/strict";
import { chromium } from "playwright";

const base = new URL(process.env.PET_TEST_URL || "http://127.0.0.1:4173").origin;
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({
  headless: true,
  ...(process.env.PET_TEST_BROWSER && { executablePath: process.env.PET_TEST_BROWSER }),
});
console.log("Chromium", browser.version());
const failures = [];
const page = await browser.newPage({ viewport: { width: 960, height: 840 } });
page.setDefaultTimeout(7000);

async function scenario(name, panel, test) {
  const errors = [];
  const recordError = error => errors.push(error.message);
  page.on("pageerror", recordError);
  try {
    await page.route("**/*", route => {
      if (new URL(route.request().url()).origin === base) return route.continue();
      errors.push("unexpected external request");
      return route.abort();
    });
    await page.route("**/__volume-location", route => route.fulfill({ contentType: "text/html",
      body: '<!doctype html><html lang="zh"><meta charset="utf-8"><body><main id="root"></main></body></html>' }));
    await page.goto(base + "/__volume-location");
    await page.evaluate(async panel => {
      const refresh = (await import("/@react-refresh")).default;
      refresh.injectIntoGlobalHook(window);
      window.$RefreshReg$ = () => {}; window.$RefreshSig$ = () => type => type;
      window.__vite_plugin_react_preamble_installed__ = true;
      await import("/src/styles.css");
      const React = (await import("/node_modules/.vite/deps/react.js")).default;
      const dom = (await import("/node_modules/.vite/deps/react-dom_client.js")).default;
      const root = dom.createRoot(document.getElementById("root"));
      const { default: Volume } = await import("/src/dashboard/DeviceVolumePanel.jsx");
      const { default: Location } = await import("/src/dashboard/SharedLocationPanel.jsx");
      const { default: Card } = await import("/src/shell/Card.jsx");
      const manual = { mode:"manual",status:"ready",source:"manual",city:"内蒙古自治区呼和浩特市土默特左旗",region:"",country:"",
        lat:40,lon:111,updatedAt:1,expiresAt:null };
      const h = window.harness = {
        wifi:{boardDeviceId:"p4-000000000001",bootId:"0000000000000001",wifiConnected:true,
          panelControl:true,controlTrusted:true,trustedVolume:true},
        usb:{connected:true,boardDeviceId:"p4-000000000003",bootId:"0000000000000003",
          portName:"fixture",runtime:"esp-p4",connectionId:"1",capabilities:{audioMasterVolume:"p4-master-volume-v1"}},
        calls:[],volumes:{},persisted:true,location:manual,unexpected:[],
        render({wifi=null,usb=null,active=true}={}) {
          root.render(React.createElement("section",{hidden:!active},
            React.createElement(Card,null,React.createElement("button",{id:"before-control"},"前一项"),
              panel==="volume" ? React.createElement(Volume,{usb,wifi,active}) : React.createElement(Location,{active}))));
        },
      };
      window.__TAURI_INTERNALS__ = { invoke:async (cmd,args={}) => {
        if(cmd==="set_exit_reasons")return;
        if(cmd==="plugin:fs|exists")return false;
        h.calls.push({cmd,...structuredClone(args)});
        if(cmd==="shared_location_status")return structuredClone(h.location);
        if(cmd==="shared_location_suggest")return [];
        if(cmd==="shared_location_configure" && args.input.mode==="off") {
          if(h.deferOff)await new Promise(resolve=>{h.finishOff=resolve;});
          h.location={...manual,mode:"off",source:null,status:"off",city:"",lat:null,lon:null};
          return structuredClone(h.location);
        }
        if(cmd==="lan_volume_request") {
          h.volumes[args.expectedBootId]=args.volume;
          return ["test-request","applied"];
        }
        if(cmd==="usb_device_volume" || (cmd==="lan_panel_control" && args.operation==="readVolume")) {
          const key=args.expectedBootId || args.expectedBoardDeviceId;
          if(args.volume!==undefined)h.volumes[key]=args.volume;
          const reply={ok:true,engine:"p4-master-volume-v1",boardDeviceId:args.boardDeviceId || args.expectedBoardDeviceId,
            volume:h.volumes[key] ?? 37,persisted:h.persisted,savePending:false};
          if(h.pauseRead===key){h.pauseRead=null;await new Promise(resolve=>{h.releaseRead=resolve;});}
          return reply;
        }
        h.unexpected.push(cmd);
        throw Error("Unexpected test IPC "+cmd);
      }};
    },panel);
    await test(page);
    assert.deepEqual(await page.evaluate(()=>harness.unexpected),[]);
    assert.deepEqual(errors,[]);
    console.log("PASS",name);
  } catch(error) {
    failures.push(name);
    console.error("FAIL",name,error);
  } finally {
    await page.goto("about:blank");
    await page.unrouteAll({behavior:"wait"});
    page.off("pageerror", recordError);
  }
}
const settle = page => page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
const wifiReads = page => page.evaluate(()=>harness.calls.filter(c=>c.operation==="readVolume").map(c=>[c.boardDeviceId,c.expectedBootId]));
const slider = page => page.getByRole("slider",{name:"设备总音量"});
async function level(page,value) {
  await page.waitForFunction(v=>{const e=document.querySelector('input[aria-label="设备总音量"]');return e && !e.disabled && e.value===String(v);},value);
}

try {
  await scenario("WLAN refresh, visibility, target and stale reply", "volume", async page=>{
    await page.evaluate(()=>harness.render({wifi:harness.wifi,active:false}));
    await settle(page);
    assert.deepEqual(await wifiReads(page),[]);
    await page.evaluate(()=>harness.render({wifi:harness.wifi}));
    await level(page,37);
    const a=["p4-000000000001","0000000000000001"];
    const b=["p4-000000000002","0000000000000002"];
    for(let i=1;i<=8;i++) {
      await page.evaluate(i=>harness.render({wifi:{...harness.wifi,httpsSuccesses:i,clockSynchronized:i%2===0}}),i);
      await settle(page);
    }
    assert.deepEqual(await wifiReads(page),[a],"unrelated renders must not read the device again");
    await page.evaluate(()=>{harness.pauseRead=harness.wifi.bootId;});
    await page.getByRole("button",{name:"读取设备音量",exact:true}).click();
    await page.waitForFunction(()=>!!harness.releaseRead);
    await page.evaluate(([boardDeviceId,bootId])=>{
      harness.wifi={...harness.wifi,boardDeviceId,bootId};
      harness.volumes[bootId]=68;
      harness.render({wifi:harness.wifi,active:false});
    },b);
    await settle(page);
    assert.deepEqual(await wifiReads(page),[a,a]);
    await page.evaluate(()=>harness.render({wifi:harness.wifi}));
    await level(page,68);
    await page.evaluate(()=>harness.releaseRead());
    await settle(page);
    assert.equal(await slider(page).inputValue(),"68","previous target's late reply");
    await page.evaluate(()=>harness.render({wifi:harness.wifi,active:false}));
    await settle(page);
    assert.deepEqual(await wifiReads(page),[a,a,b],"hiding the current target must not read it");
    await page.evaluate(()=>harness.render({wifi:harness.wifi}));
    await level(page,68);
    assert.deepEqual(await wifiReads(page),[a,a,b,b]);
    const reboot=[b[0],"0000000000000004"];
    await page.evaluate(bootId=>{
      harness.wifi={...harness.wifi,bootId}; harness.volumes[bootId]=24;
      harness.render({wifi:harness.wifi});
    },reboot[1]);
    await level(page,24);
    assert.deepEqual(await wifiReads(page),[a,a,b,b,reboot]);
  });

  for(const route of ["usb","wifi"]) {
    await scenario(route+" slider: Tab and Shift+Tab never write, arrow applies once","volume",async page=>{
      // A persisted=true reply can conceal indiscriminate keyup handlers because
      // the controller skips an already-saved value. Exercise the unpersisted state.
      await page.evaluate(route=>{harness.persisted=false;harness.render({[route]:harness[route]});},route);
      await level(page,37);
      const writes=()=>page.evaluate(()=>harness.calls.filter(c=>c.cmd==="lan_volume_request" ||
        (c.cmd==="usb_device_volume" && c.volume!==undefined)).map(c=>c.volume));
      await page.locator("#before-control").focus();
      await page.keyboard.press("Tab");
      assert.equal(await slider(page).evaluate(e=>e===document.activeElement),true);
      await settle(page);
      assert.deepEqual(await writes(),[],"Tab keyup arriving on slider must not apply the existing value");
      await page.keyboard.press("ArrowRight");
      await level(page,38);
      assert.deepEqual(await writes(),[38]);
      await page.getByRole("button",{name:"静音",exact:true}).focus();
      await page.keyboard.press("Shift+Tab");
      assert.equal(await slider(page).evaluate(e=>e===document.activeElement),true);
      await settle(page);
      assert.deepEqual(await writes(),[38],"focus navigation must not resend the arrow change");
    });
  }

  await scenario("location revoke, cancel and narrow panel geometry","location",async page=>{
    await page.evaluate(()=>harness.render());
    const region=page.getByRole("region",{name:"地区位置"});
    const city=region.getByRole("combobox");
    const off=region.getByRole("button",{name:"关闭位置共享"});
    const undo=region.getByRole("button",{name:"撤销修改"});
    const saved="内蒙古自治区呼和浩特市土默特左旗";
    await page.waitForFunction(s=>document.querySelector('.shared-location__city')?.textContent===s,saved);
    await city.fill("杭州");
    await undo.waitFor();
    const geometry=()=>region.evaluate(el=>{
      const rect=e=>e.getBoundingClientRect();
      const first=el.querySelector(".shared-location__current");
      const form=el.querySelector("form");
      const field=form.querySelector("input");
      const label=first.querySelector(".shared-location__city");
      return {width:rect(el).width,inside:[...el.querySelectorAll("input,button,label,span,strong")].every(c=>
        rect(c).left>=rect(el).left-1 && rect(c).right<=rect(el).right+1),
        fieldWidth:rect(field).width,cityVisible:label.scrollWidth<=label.clientWidth+1,
        buttonsBelow:rect(first.querySelector("button")).top>=rect(label).bottom &&
          rect(form.querySelector("button")).top>=rect(field).bottom,
        wideInline:rect(form.querySelector("button")).top<rect(field).bottom};
    });
    for(const width of [320,390,960]){
      await page.setViewportSize({width,height:840});await settle(page);
      const g=await geometry();
      console.log("layout",width,g);
      assert.ok(g.width<=width && g.inside && g.fieldWidth>=130 && g.cityVisible);
      assert.ok(width<400 ? g.buttonsBelow : g.wideInline);
    }
    // A settings column can also be narrow inside a wide desktop window.
    await page.locator("#root").evaluate(el => { el.style.width = "320px"; });
    await settle(page);
    const column = await geometry();
    console.log("layout 960, column 320", column);
    assert.ok(column.width <= 320 && column.inside && column.fieldWidth >= 130 && column.cityVisible && column.buttonsBelow);
    await page.locator("#root").evaluate(el => { el.style.removeProperty("width"); });
    await page.setViewportSize({width:320,height:840});
    await undo.click();
    assert.equal(await city.inputValue(),saved);
    assert.equal(await undo.count(),0);
    const configurations=()=>page.evaluate(()=>harness.calls.filter(c=>c.cmd==="shared_location_configure").map(c=>c.input));
    assert.deepEqual(await configurations(),[]);
    await page.evaluate(()=>{harness.deferOff=true;});
    await off.click();
    await page.waitForFunction(()=>!!harness.finishOff);
    assert.equal(await off.isDisabled(),true);
    assert.equal(await region.locator(".shared-location__city").innerText(),saved);
    await page.evaluate(()=>harness.finishOff());
    await region.getByText("未开启",{exact:true}).waitFor();
    assert.deepEqual(await configurations(),[{mode:"off",manual:null}]);
    assert.equal(await city.inputValue(),"");
    assert.equal(await off.count(),0);
    await city.fill("北京");
    await undo.click();
    assert.equal(await city.inputValue(),"");
    assert.deepEqual(await configurations(),[{mode:"off",manual:null}]);
  });
} finally {await browser.close();}
if(failures.length)process.exitCode=1;
