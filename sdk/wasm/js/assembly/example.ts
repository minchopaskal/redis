import {
  callCommand,
  error,
  readInput,
  registerFunction,
  replyRaw,
  status,
} from "./sdk";

export function redis_wasm_abi_version_0_1_0(): void {}

// AssemblyScript exports its runtime initializer as _initialize. Redis calls
// main only after that initializer has completed.
export function main(_argc: i32, _argv: i32): i32 {
  if (!registerFunction("js_echo", "js_echo")) unreachable();
  if (!registerFunction("js_incr", "js_incr")) unreachable();
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
