import { pollWhileVisible } from "../lib/visible-poll.js";
import React, { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import Card from "../shell/Card.jsx";
import Button from "../shell/Button.jsx";
import Switch from "../shell/Switch.jsx";
import P4BoardInputMap, { ControlGlyph, P4_MAP_CONTROLS } from "./P4BoardInputMap.jsx";
import { friendlyControlLabel } from "../lib/usage-help.js";
import { useExitProtection } from "../lib/exit-protection.js";
import {
  BLUETOOTH_SHORTCUT_KEYS, BLUETOOTH_KEYBOARD_KEYS, BLUETOOTH_KEYBOARD_MODIFIERS,
  isKeyboardChordSupported, keyboardChordKey, readKeyboardSnapshot, storeKeyboardSnapshot, verifiedKeyboardState,
} from "../lib/bluetooth-shortcut.js";

const pressGestures = ["单击", "双击", "长按"];
const joystickGestures = [
  { label: "中按短按", slot: 9 },
  { label: "中按长按", slot: 11 },
  { label: "上", slot: 12 },
  { label: "下", slot: 14 },
  { label: "左", slot: 16 },
  { label: "右", slot: 18 },
];
const controls = P4_MAP_CONTROLS.map((control, index) => ({
  ...control,
  gestures: control.id === "p4_joystick" ? joystickGestures
    : pressGestures.map((label, offset) => ({ label, slot: index * 3 + offset })),
}));
const gestureCount = controls.reduce((count, group) => count + group.gestures.length, 0);

function chordLabel(chord) {
  if (chord == null) return "未读取";
  if (!chord) return "未设置";
  const key = BLUETOOTH_KEYBOARD_KEYS.find(({ usage }) => usage === (chord & 255));
  const modifiers = BLUETOOTH_KEYBOARD_MODIFIERS.filter(({ mask }) => (chord >> 8) & mask);
  return [...modifiers.map(({ label }) => label), key?.label].filter(Boolean).join(" + ");
}

function connectionLabel(config) {
  if (!config.enabled) return "蓝牙模式已关闭，设备按键配置生效。";
  if (config.connected) return "蓝牙键盘已连接，设备按键配置暂停。";
  if (config.radioState < 0) return "蓝牙启动失败，设备按键配置仍可使用。";
  if (config.pairing) return "请在电脑的蓝牙设置中选择 HachimiDock。连接前，设备按键配置仍可使用。";
  return config.paired ? "正在等待已配对的电脑连接，设备按键配置仍可使用。"
    : "配对等待已结束，设备按键配置仍可使用。";
}

export default function BluetoothShortcutPanel({ usb, wifi, board, active = true }) {
  const wired = usb?.connected === true;
  const connection = wired ? usb.connectionId : wifi?.bootId;
  const supported = wired ? usb.capabilities?.bluetoothKeyboardConfig === "p4-ble-keyboard-config-v1"
    : Boolean(wifi?.wifiConnected && wifi.controlTrusted && wifi.bootId && wifi.boardDeviceId === board);
  const extended = wired ? usb.capabilities?.bluetoothKeyboardExtendedChords === true : supported;
  const modifierOptions = BLUETOOTH_KEYBOARD_MODIFIERS.filter(({mask}) => extended || mask <= 8);
  const keyOptions = extended ? BLUETOOTH_KEYBOARD_KEYS : BLUETOOTH_SHORTCUT_KEYS;
  const [capture, setCapture] = useState(false);
  const captureInput = useRef(null);
  const pressed = useRef(new Map());
  const captured = useRef(null);
  const captureInvalid = useRef(false);
  const [config, setConfig] = useState(null);
  const [readError, setReadError] = useState("");
  const [snapshot] = useState(() => readKeyboardSnapshot(board));
  const [slot, setSlot] = useState(0);
  const [hoveredControl, setHoveredControl] = useState("");
  const groups = useRef(new Map());
  const [edit, setEdit] = useState(null);
  const [pending, setPending] = useState(false);
  const [notice, setNotice] = useState(null);
  const canEdit = supported && Boolean(config) && !readError;
  const visibleKeys = config?.keys ?? snapshot?.keys;
  const saved = config?.keys?.[slot] || 0;
  const chord = edit?.chord ?? saved;
  const conflicted = Boolean(edit && edit.base !== saved && chord !== saved);
  function draft(value) {
    setEdit(previous => ({ base: previous && previous.chord !== saved ? previous.base : saved, chord: value }));
  }
  const dirty = chord !== saved;
  const mask = chord >> 8;
  const usage = chord & 255;
  const activeControl = controls.find(group => group.gestures.some(gesture => gesture.slot === slot))?.id;
  useExitProtection(pending ? "正在保存蓝牙快捷键" : dirty ? "蓝牙快捷键有未保存的修改" : "");

  const requestConfig = useCallback(async (change = {}, conditional) => {
    const payload = conditional ? { conditional } : change;
    const state = wired
      ? await invoke("usb_bluetooth_keyboard_config", { boardDeviceId: board, connectionId: connection, ...payload })
      : await invoke("lan_bluetooth_keyboard_config", { boardDeviceId: board, expectedBootId: connection, ...payload });
    if (conditional && (state?.conditionalUpdate !== "keyboard-cas-v1" || state.conditionalApplied !== true)) {
      throw new Error("设备未确认条件更新，请重新读取配置。");
    }
    return verifiedKeyboardState(state, board, change);
  }, [board, connection, wired]);

  const session = useRef(0);
  const saving = useRef(false);
  useLayoutEffect(() => () => { session.current++; }, [board, connection, wired, active, supported]);

  useEffect(() => {
    if (!active || !supported || !board || !connection || pending) return;
    let cancelled = false;
    let reading = false;
    async function read() {
      if (reading) return;
      reading = true;
      try {
        const state = await requestConfig();
        if (!cancelled && !saving.current) {
          setConfig(state);
          setReadError("");
        }
      } catch (error) {
        if (!cancelled && !saving.current) setReadError(error?.message || String(error));
      } finally {
        reading = false;
      }
    }
    void read();
    const stop = pollWhileVisible(read, 2000);
    return () => { cancelled = true; stop(); };
  }, [active, board, connection, supported, pending, requestConfig]);

  useEffect(() => {
    if (!active) setCapture(false);
  }, [active]);

  useEffect(() => {
    if (canEdit) storeKeyboardSnapshot(board, config.keys);
  }, [board, canEdit, config?.keys]);

  useEffect(() => {
    if (capture) captureInput.current?.focus();
  }, [capture]);

  useEffect(() => {
    setCapture(false);
    setEdit(null);
  }, [board, connection]);

  function startCapture() {
    pressed.current.clear();
    captured.current = null;
    captureInvalid.current = false;
    setNotice(null);
    setCapture(true);
  }

  function captureDown(event) {
    event.preventDefault();
    event.stopPropagation();
    if (event.repeat || pressed.current.has(event.code)) return;
    const key = keyboardChordKey(event.code, extended);
    pressed.current.set(event.code, key);
    if (!key) {
      captureInvalid.current = true;
      setNotice({ error: true, text: "这个键当前无法录入，请选择其它键或手动配置。" });
      return;
    }
    let mods = 0;
    for (const down of pressed.current.values()) mods |= down?.mask || 0;
    if (key.usage) {
      if (captured.current?.usage) {
        captureInvalid.current = true;
        setNotice({ error: true, text: "一个快捷键只能包含一个主键，请重新录入。" });
        return;
      }
      captured.current = { mask: mods, usage: key.usage };
    } else if (captured.current?.usage && [...pressed.current.values()].some(down => down?.usage)) {
      captured.current.mask |= mods;
    } else if (!captured.current?.usage) {
      captured.current = { mask: mods, usage: 0 };
    }
  }

  function captureUp(event) {
    event.preventDefault();
    event.stopPropagation();
    if (!pressed.current.delete(event.code) || pressed.current.size) return;
    const result = captured.current;
    const next = result ? (result.mask << 8) | result.usage : 0;
    if (!captureInvalid.current && next && isKeyboardChordSupported(next, extended)) {
      draft(next);
      setNotice({ text: `已录入 ${chordLabel(next)}，保存后生效。` });
      setCapture(false);
    } else {
      if (!captureInvalid.current) setNotice({ error: true, text: "当前固件需要修饰键和主键。请选择组合键，或升级固件。" });
      captured.current = null;
      captureInvalid.current = false;
    }
  }

  async function save(change) {
    if (!active || !canEdit || saving.current ||
        ("chord" in change && !isKeyboardChordSupported(change.chord, extended))) return;
    const owner = session.current;
    saving.current = true;
    setPending(true);
    setNotice(null);
    try {
      const latest = await requestConfig();
      if (owner !== session.current) return;
      setConfig(latest);
      if ("slot" in change && latest.keys[change.slot] !== (edit?.base ?? saved)) {
        throw new Error("该手势已在设备上更改，请核对最新映射后再编辑。");
      }
      if ("enabled" in change && (latest.enabled !== config.enabled ||
          (change.enabled && latest.enabled && (latest.connected || latest.pairing || latest.paired)))) {
        throw new Error("设备的蓝牙模式或配对状态已变化，请重新核对。");
      }
      const supportsConditional = latest.conditionalUpdate === "keyboard-cas-v1";
      if (config.conditionalUpdate === "keyboard-cas-v1" && !supportsConditional) {
        throw new Error("设备的条件更新能力已变化，请重新核对配置。");
      }
      let conditional;
      if (supportsConditional) {
        conditional = "slot" in change
          ? { slot: change.slot, setChord: change.chord, expectedChord: latest.keys[change.slot] }
          : { setEnabled: change.enabled, expectedEnabled: latest.enabled };
      }
      const state = await requestConfig(change, conditional);
      if (owner !== session.current) return;
      setConfig(state);
      if ("slot" in change) {
        setEdit(null);
        setNotice({ text: change.chord ? "快捷键已保存。" : "快捷键已清除。" });
      }
    } catch (error) {
      if (owner !== session.current) return;
      setReadError("正在重新读取设备键盘配置…");
      setNotice({ error: true, text: error?.message || String(error) });
    } finally {
      saving.current = false;
      setPending(false);
    }
  }

  return <Card title="蓝牙快捷键" subtitle="把设备变成电脑的蓝牙快捷键盘。"
    actions={canEdit ? <Switch label="蓝牙模式" checked={config.enabled} disabled={pending}
      onCheckedChange={enabled => void save({ enabled })} /> : <span>蓝牙模式状态未确认</span>}>
    <p>开启蓝牙模式后，在电脑的蓝牙设置中配对并连接 HachimiDock。连接成功后，设备本身的按键功能暂停，实体按键和摇杆只向电脑发送这里配置的快捷键。</p>
    <p>等待连接、断开蓝牙或关闭蓝牙模式时，设备本身的按键功能会生效。初始配置中，1、2、3 键单击分别发送 Ctrl + C、Ctrl + V、Ctrl + A。为某个键设置双击后，该键的单击会等待约 280 毫秒。清除双击映射即可恢复即时单击。</p>
    {!canEdit && <p role="status">
      {!wired && !wifi ? "可离线查看快捷键。通过 USB 或已配对的 Wi-Fi 连接设备后即可修改。"
        : !wired && wifi?.controlTrusted !== true ? "请通过 USB 连接同一台设备，授权 Wi-Fi 控制后再修改快捷键。"
        : !supported ? "当前设备固件尚未开放独立的蓝牙键盘配置。"
        : readError || "正在读取配置…"}
      {visibleKeys && ` 当前显示上次读取的设备配置（${config ? board : snapshot.board}）。`}
    </p>}
    <form className="api-settings__form" onSubmit={event => { event.preventDefault(); void save({ slot, chord }); }}>
      {canEdit && <>
        <p role="status">{pending ? "正在等待设备确认…" : connectionLabel(config)}</p>
        {config.passkey > 0 && <p>配对码 <strong>{String(config.passkey).padStart(6, "0")}</strong></p>}
        {config.enabled && !config.connected && !config.paired && !config.pairing && config.radioState >= 0 &&
          <Button disabled={pending} onClick={() => void save({ enabled: true })}>重新进入配对</Button>}
      </>}
        <div className="board-button-panel__workspace board-button-panel__workspace--p4">
          <section className="board-button-panel__left" aria-label="蓝牙控件导航">
            <div className="board-button-panel__device-stage">
              <P4BoardInputMap gestureCount={gestureCount} activeControl={activeControl}
                highlightedControl={hoveredControl} onHover={setHoveredControl}
                onSelect={id => {
                  const group = controls.find(control => control.id === id);
                  if (!pending && !dirty && !capture) {
                    setSlot(group.gestures[0].slot);
                    setEdit(null);
                  }
                  groups.current.get(dirty || capture ? activeControl : id)?.scrollIntoView({ block: "nearest" });
                }} />
            </div>
          </section>
          <section className="board-button-panel__right" aria-label="蓝牙实体控件与手势配置">
            <div className="board-button-control-groups">
              {controls.map(group => (
                <fieldset className={`board-button-control-group board-button-control-group--${group.gestures.length}`}
                  key={group.id} ref={node => {
                    if (node) groups.current.set(group.id, node);
                    else groups.current.delete(group.id);
                  }}
                  onMouseEnter={() => setHoveredControl(group.id)} onMouseLeave={() => setHoveredControl("")}>
                  <legend><small>{group.label} · {group.detail}</small></legend>
                  <div className="board-button-control-group__visual">
                    <ControlGlyph controlId={group.id} label={group.label} active={activeControl === group.id} />
                    <span><strong>{friendlyControlLabel(group.label)}</strong></span>
                  </div>
                  <div className={`board-button-control-group__rows board-button-control-group__rows--${group.gestures.length}`}>
                    {group.gestures.map(gesture => (
                      <div className={`board-button-control-field${slot === gesture.slot ? " is-hovered" : ""}`} key={gesture.slot}>
                        <span className="board-button-control-field__head"><strong>{gesture.label}</strong></span>
                        <Button size="small" aria-expanded={canEdit && slot === gesture.slot}
                          disabled={pending || ((dirty || capture) && slot !== gesture.slot)}
                          onClick={() => { if (slot !== gesture.slot) { setSlot(gesture.slot); setEdit(null); } }}>
                          {chordLabel(visibleKeys?.[gesture.slot])}
                        </Button>
                        {canEdit && slot === gesture.slot && <div className="bluetooth-shortcut-editor">
                          {capture ? <div className="bluetooth-shortcut-actions">
                            <input ref={captureInput} readOnly aria-label="录入组合键" placeholder="按下组合键，然后松开"
                              onKeyDown={captureDown} onKeyUp={captureUp} onBlur={() => setCapture(false)} />
                            <Button onClick={() => setCapture(false)}>取消录入</Button>
                          </div> : <Button disabled={pending} onClick={startCapture}>录入组合键</Button>}
                          <fieldset className="bluetooth-shortcut-chord" disabled={pending || capture}>
                            <legend>电脑组合键</legend>
                            <div className="bluetooth-shortcut-chord__modifiers">
                              {modifierOptions.map(modifier => <label key={modifier.mask}>
                                <input type="checkbox" checked={Boolean(mask & modifier.mask)}
                                  onChange={() => draft(((mask ^ modifier.mask) << 8) | usage)} />
                                {modifier.label}
                              </label>)}
                            </div>
                            <label>主键
                              <select aria-label="主键" value={usage} onChange={event => draft((mask << 8) | Number(event.target.value))}>
                                <option value={0}>{extended ? "空（只发送修饰键）" : "选择一个键"}</option>
                                {keyOptions.map(key => <option key={key.usage} value={key.usage}>{key.label}</option>)}
                              </select>
                            </label>
                          </fieldset>
                          <div className="bluetooth-shortcut-actions">
                            {conflicted && <p role="status">设备上的同一手势已更改。取消修改可载入最新映射。</p>}
                            <Button type="submit" variant="primary" disabled={conflicted || !dirty || capture || !chord || !isKeyboardChordSupported(chord, extended)} loading={pending} loadingLabel="正在保存…">保存快捷键</Button>
                            {dirty && <Button disabled={pending || capture} onClick={() => setEdit(null)}>取消修改</Button>}
                            {saved !== 0 && <Button disabled={pending || capture || conflicted} onClick={() => void save({ slot, chord: 0 })}>清除快捷键</Button>}
                          </div>
                        </div>}
                      </div>
                    ))}
                  </div>
                </fieldset>
              ))}
            </div>
          </section>
        </div>
    </form>
    {notice && <p role={notice.error ? "alert" : "status"}>{notice.text}</p>}
  </Card>;
}
