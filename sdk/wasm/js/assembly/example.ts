import {
  callCommand,
  error,
  readInput,
  registerFunction,
  replyRaw,
  status,
} from "./sdk";

export function redis_abi_version(): i32 {
  return 1;
}

export function redis_init(): i32 {
  if (!registerFunction("js_echo", "js_echo")) return 1;
  if (!registerFunction("js_incr", "js_incr")) return 2;
  return 0;
}

export function js_echo(): i32 {
  const input = readInput();
  if (input == null || input.keysCount != 0 || input.argsCount != 1) {
    error("js_echo expects one argument");
    return 0;
  }
  const value = input.arg(0);
  status(String.UTF8.decodeUnsafe(value.dataStart, value.length, false));
  return 0;
}

export function js_incr(): i32 {
  const input = readInput();
  if (input == null || input.keysCount != 1 || input.argsCount != 0) {
    error("js_incr expects one key");
    return 0;
  }
  const args = new Array<Uint8Array>();
  args.push(input.key(0));
  const reply = callCommand("INCR", args);
  if (reply == null || !replyRaw(reply)) {
    error("INCR failed");
  }
  return 0;
}
