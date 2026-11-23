// For display-only reads. Transport heartbeats and pending operation receipts use their own clocks.
export function pollWhileVisible(read, interval, page = document, clock = globalThis) {
  let timer;
  function update(refresh) {
    if (timer !== undefined) clock.clearInterval(timer);
    timer = undefined;
    if (page.visibilityState === "hidden") return;
    if (refresh) read();
    timer = clock.setInterval(read, interval);
  }
  const onVisibility = () => update(true);
  page.addEventListener("visibilitychange", onVisibility);
  update(false);
  return () => {
    page.removeEventListener("visibilitychange", onVisibility);
    if (timer !== undefined) clock.clearInterval(timer);
  };
}
