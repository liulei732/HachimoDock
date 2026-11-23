const fs = require("node:fs");

const CHUNK = 64 * 1024;
const POLL_BYTES = 512 * 1024;
const MAX_LINE = 2 * 1024 * 1024;
const ANCHOR = 96;

function identity(stat) {
  return stat.dev + ":" + stat.ino;
}

function previousBytes(fd, offset) {
  const bytes = Buffer.allocUnsafe(Math.min(offset, ANCHOR));
  let read = 0;
  while (read < bytes.length) {
    const n = fs.readSync(fd, bytes, read, bytes.length - read, offset - bytes.length + read);
    if (!n) break;
    read += n;
  }
  return bytes.subarray(0, read);
}

// A long line can arrive one byte at a time. Retain one growing buffer so
// its object count is bounded as well as its byte length.
function appendFragment(entry, part) {
  const size = entry.fragmentBytes + part.length;
  if (!entry.fragment || size > entry.fragment.length) {
    const capacity = Math.min(MAX_LINE, Math.max(size, (entry.fragment?.length || 128) * 2));
    const next = Buffer.allocUnsafe(capacity);
    if (entry.fragmentBytes) entry.fragment.copy(next, 0, 0, entry.fragmentBytes);
    entry.fragment = next;
  }
  part.copy(entry.fragment, entry.fragmentBytes);
  entry.fragmentBytes = size;
  return entry.fragment.subarray(0, size);
}

// Stop after a bounded slice; the caller owns the cursor across polls. A
// changed tail anchor catches truncate-and-regrow even within the same inode.
function readLines(path, stat, entry, replace, consume, budget = POLL_BYTES) {
  const changed = s => entry.offset > s.size ||
    (entry.fileId != null && entry.fileId !== identity(s));
  if (!changed(stat) && entry.offset === stat.size &&
      entry.mtime === stat.mtimeMs && entry.ctime === stat.ctimeMs) return;
  const fd = fs.openSync(path, "r");
  try {
    stat = fs.fstatSync(fd);
    if (changed(stat) || (entry.anchor?.length &&
        !previousBytes(fd, entry.offset).equals(entry.anchor))) entry = replace();
    entry.fileId = identity(stat);
    entry.mtime = stat.mtimeMs;
    entry.ctime = stat.ctimeMs;
    const endOffset = Math.min(stat.size, entry.offset + budget);
    const scratch = Buffer.allocUnsafe(Math.min(CHUNK, endOffset - entry.offset));
    entry.fragmentBytes ??= 0;
    if (!entry.anchor && entry.offset) entry.anchor = previousBytes(fd, entry.offset);
    while (entry.offset < endOffset) {
      const n = fs.readSync(fd, scratch, 0, Math.min(scratch.length, endOffset - entry.offset), entry.offset);
      if (!n) break;
      entry.offset += n;
      const bytes = scratch.subarray(0, n);
      entry.anchor = n >= ANCHOR ? Buffer.from(bytes.subarray(n - ANCHOR)) :
        Buffer.concat([entry.anchor || Buffer.alloc(0), bytes]).subarray(-ANCHOR);
      let start = 0;
      while (start < n) {
        const newline = bytes.indexOf(10, start);
        const stop = newline < 0 ? n : newline;
        const part = bytes.subarray(start, stop);
        if (!entry.dropFirstLine && entry.fragmentBytes + part.length > MAX_LINE) {
          entry.oversizeLines = (entry.oversizeLines || 0) + 1;
          entry.dropFirstLine = true;
          entry.fragment = null;
          entry.fragmentBytes = 0;
        }
        if (newline < 0) {
          if (!entry.dropFirstLine) {
            appendFragment(entry, part);
          }
          break;
        }
        if (!entry.dropFirstLine) {
          const line = (entry.fragmentBytes ? appendFragment(entry, part) : part).toString("utf8").trim();
          if (line) consume(line, entry, entry.offset - n + newline + 1);
        }
        entry.dropFirstLine = false;
        entry.fragment = null;
        entry.fragmentBytes = 0;
        start = newline + 1;
      }
    }
  } finally {
    fs.closeSync(fd);
  }
}

module.exports = { identity, readLines };
