import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { sendTestMessage, testMessageTarget } from "./lib/device-test-message.js";
import Button from "./shell/Button.jsx";
import { wifiDeviceControl, wifiDeviceControlReady } from "./lib/wifi-device-control.js";
import { GUIDE_OFFLINE_APPS, GUIDE_TEST_MESSAGE } from "./lib/device-guide-content.js";

const pages = { main: "桌宠主屏", components: "组件菜单", app: "组件页面" };

function checkReply(reply, board, boot) {
  if (reply?.ok !== true || reply.boardDeviceId !== board || (reply.bootId != null && boot && reply.bootId !== boot)) {
    throw new Error("未取得当前设备的回读，请重试。");
  }
}

export default function DeviceGuidePractice({ usb, wifi, runtime }) {
  const wireless = !usb?.connected && wifi?.wifiConnected === true;
  const board = usb?.connected && !usb.boardWedged ? usb.boardDeviceId : wireless ? wifi.boardDeviceId : "";
  const boot = wireless ? wifi.bootId : usb?.bootId || usb?.capabilities?.debugBootId;
  const p4 = String(runtime).toLowerCase() === "esp-p4";
  const canRead = board && p4 && (!wireless || wifiDeviceControlReady(wifi));
  const [feedback, setFeedback] = useState("");
  const [offline, setOffline] = useState("");
  const [sent, setSent] = useState("");
  const target = testMessageTarget(usb, wifi, board);
  const [pending, setPending] = useState("");
  const [notice, setNotice] = useState("");
  const lastPage = useRef("");
  const live = useRef(false);
  const busy = useRef(false);

  useEffect(() => {
    live.current = true;
    return () => { live.current = false; };
  }, []);

  async function run(action, work) {
    if (busy.current) return;
    busy.current = true;
    setPending(action);
    setNotice("");
    try {
      await work();
    } catch (error) {
      if (live.current) setNotice(action === "send"
        ? String(error?.message || error)
        : "这次未能核对设备状态。可以重试，也可以按屏幕实际情况手动确认。");
    } finally {
      busy.current = false;
      if (live.current) setPending("");
    }
  }

  function read() {
    return run("read", async () => {
      const reply = wireless ? await wifiDeviceControl(invoke, wifi, "readDiagnostics")
        : await invoke("usb_get_diagnostics", { expectedBoardDeviceId: board });
      if (!live.current) return;
      checkReply(reply, board, boot);
      const page = reply.runtime?.screenPage;
      if (!Object.hasOwn(pages, page)) {
        lastPage.current = "";
        setNotice("设备暂未提供可核对的页面，可按屏幕实际情况确认。");
        return;
      }
      const previous = lastPage.current;
      lastPage.current = page;
      if (previous && page !== previous) setFeedback("设备回读：" + pages[previous] + " → " + pages[page]);
      setNotice("设备当前页面：" + pages[page]);
      if (page !== "app") return;
      if (reply.runtime.miniappActive === false) {
        setNotice("设备已进入组件页面，尚未报告有组件在运行。");
        return;
      }
      const inventory = wireless ? await wifiDeviceControl(invoke, wifi, "readComponents")
        : await invoke("list_device_widgets", { input: { transport: "usb", boardDeviceId: board, sshHost: "" } });
      if (!live.current) return;
      checkReply(inventory, board, boot);
      const id = inventory.activeWidgetId;
      const complete = wireless ? inventory.complete === true && !inventory.recoveryBlocked
        : inventory.freshness === "live" && inventory.warnings?.length === 0;
      if (complete && Object.hasOwn(GUIDE_OFFLINE_APPS, id)
          && inventory.items?.some(item => item.id === id && item.active === true && item.manifestState === "valid")) {
        setOffline("设备回读：" + GUIDE_OFFLINE_APPS[id] + "已打开");
      } else {
        setNotice("设备已进入组件页面，尚未核对到本次体验的离线组件。");
      }
    });
  }

  function send() {
    return run("send", async () => {
      setSent("");
      const receipt = await sendTestMessage(invoke, target);
      if (live.current) setSent(receipt);
    });
  }

  const completed = [board, feedback, offline].filter(Boolean).length;
  return (
    <div className="device-guide-card">
      <p className="device-guide-headline">无需模型 Key，在桌宠上试试这三步。可随时结束。</p>
      <strong aria-live="polite">已体验 {completed}/3 项</strong>
      <ol className="device-guide-control-rows">
        <li><strong>连上设备</strong><p role="status">{board
          ? (wireless ? "Wi-Fi" : "USB") + " 已连接 · " + board
          : "等待设备连接。连接 USB 或在设备设置中连接已配对的 Wi-Fi 设备。"}</p></li>
        <li><strong>看见屏幕反馈</strong>
          <p>{target ? `在桌宠主屏发送测试消息，看看是否出现「${GUIDE_TEST_MESSAGE}」气泡。`
            : "也可在设备上短按 SW2 切换页面，观察屏幕反馈。"}</p>
          {target && <Button size="small" disabled={!!pending} onClick={send}>
            {pending === "send" ? "正在发送…" : sent ? "再次发送测试消息" : "发送测试消息"}
          </Button>}
          <p role="status">{feedback || sent || "等待屏幕反馈"}</p>
          {!feedback && <Button size="small" disabled={!board} onClick={() => setFeedback("你已确认看到屏幕反馈")}>我看到了</Button>}
        </li>
        <li><strong>打开离线组件</strong>
          <p>{p4 ? "短按 SW2 到组件菜单，用摇杆选「静心木鱼」或「像素贪吃蛇」，短按 SW1 打开。"
            : "在设备上打开一个已安装的离线组件，进入后可在这里确认。"}</p>
          <p role="status">{offline || "等待打开离线组件"}</p>
          {!offline && <Button size="small" disabled={!board} onClick={() => setOffline("你已确认打开离线组件")}>我已打开离线组件</Button>}
        </li>
      </ol>
      {canRead ? <>
        <p>先读取当前页面，切屏或打开组件后再读一次。</p>
        <Button size="small" disabled={!!pending} onClick={read}>{pending === "read" ? "正在读取…" : "读取设备页面"}</Button>
      </> : board ? <p>当前连接未提供页面回读，手动确认可跳过。</p> : null}
      {notice && <p role="status">{notice}</p>}
    </div>
  );
}
