@external("redis", "input_len")
declare function hostInputLen(): i32;
@external("redis", "input_read")
declare function hostInputRead(dst: usize, cap: i32): i32;
@external("redis", "keys_count")
declare function hostKeysCount(): i32;
@external("redis", "call")
declare function hostCall(argv: usize, len: i32): i32;
@external("redis", "reply_read")
declare function hostReplyRead(dst: usize, cap: i32, offset: i32): i32;
@external("redis", "reply_raw")
declare function hostReplyRaw(reply: usize, len: i32): i32;
@external("redis", "status_reply")
declare function hostStatusReply(message: usize, len: i32): void;
@external("redis", "error_reply")
declare function hostErrorReply(message: usize, len: i32): void;
@external("redis", "register_function")
declare function hostRegisterFunction(
  name: usize,
  nameLen: i32,
  exportName: usize,
  exportLen: i32,
): i32;

export class Input {
  keysCount: i32 = 0;
  items: Array<Uint8Array> = new Array<Uint8Array>();

  get count(): i32 {
    return this.items.length;
  }

  get argsCount(): i32 {
    return this.items.length - this.keysCount;
  }

  key(index: i32): Uint8Array {
    return this.items[index];
  }

  arg(index: i32): Uint8Array {
    return this.items[this.keysCount + index];
  }
}

function utf8(value: string): Uint8Array {
  return Uint8Array.wrap(String.UTF8.encode(value, false));
}

export function readInput(): Input | null {
  const length = hostInputLen();
  const keys = hostKeysCount();
  if (length < 4 || keys < 0) return null;

  const packed = new Uint8Array(length);
  if (hostInputRead(packed.dataStart, length) != length) return null;
  const count = load<u32>(packed.dataStart);
  if (keys > <i32>count) return null;

  const result = new Input();
  result.keysCount = keys;
  let cursor: i32 = 4;
  for (let i: u32 = 0; i < count; i++) {
    if (cursor > length - 4) return null;
    const itemLength = load<u32>(packed.dataStart + cursor);
    cursor += 4;
    if (itemLength > <u32>(length - cursor)) return null;
    const item = new Uint8Array(itemLength);
    memory.copy(item.dataStart, packed.dataStart + cursor, itemLength);
    result.items.push(item);
    cursor += itemLength;
  }
  return cursor == length ? result : null;
}

export function registerFunction(name: string, exportName: string): bool {
  const nameBytes = utf8(name);
  const exportBytes = utf8(exportName);
  return hostRegisterFunction(
    nameBytes.dataStart,
    nameBytes.length,
    exportBytes.dataStart,
    exportBytes.length,
  ) == 0;
}

export function call(argv: Array<Uint8Array>): Uint8Array | null {
  if (argv.length == 0) return null;
  let packedLength: i32 = 4;
  for (let i = 0; i < argv.length; i++) {
    packedLength += 4 + argv[i].length;
  }
  const packed = new Uint8Array(packedLength);
  store<u32>(packed.dataStart, argv.length);
  let cursor: i32 = 4;
  for (let i = 0; i < argv.length; i++) {
    const item = argv[i];
    store<u32>(packed.dataStart + cursor, item.length);
    cursor += 4;
    memory.copy(packed.dataStart + cursor, item.dataStart, item.length);
    cursor += item.length;
  }

  const replyLength = hostCall(packed.dataStart, packed.length);
  if (replyLength < 0) return null;
  const reply = new Uint8Array(replyLength);
  if (hostReplyRead(reply.dataStart, reply.length, 0) != replyLength)
    return null;
  return reply;
}

export function callCommand(command: string, args: Array<Uint8Array>): Uint8Array | null {
  const argv = new Array<Uint8Array>();
  argv.push(utf8(command));
  for (let i = 0; i < args.length; i++) argv.push(args[i]);
  return call(argv);
}

export function replyRaw(reply: Uint8Array): bool {
  return hostReplyRaw(reply.dataStart, reply.length) == 0;
}

export function status(message: string): void {
  const bytes = utf8(message);
  hostStatusReply(bytes.dataStart, bytes.length);
}

export function error(message: string): void {
  const bytes = utf8(message);
  hostErrorReply(bytes.dataStart, bytes.length);
}

export function redisAbort(
  message: string | null,
  fileName: string | null,
  line: u32,
  column: u32,
): void {
  unreachable();
}
