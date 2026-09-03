#![no_std]

use core::panic::PanicInfo;
use redis_wasm::{
    blob_read, blob_write, error, parse_i32, read_input, register_blob_type, register_function,
    reply_integer, status, Slice, ABI_VERSION,
};

const TYPE_NAME: &str = "rust_hashmap";
const CAPACITY: usize = 16;
const HEADER_SIZE: usize = 12;
const SLOT_SIZE: usize = 13;
const PAYLOAD_SIZE: usize = HEADER_SIZE + CAPACITY * SLOT_SIZE;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    bytes[offset] as u32
        | (bytes[offset + 1] as u32) << 8
        | (bytes[offset + 2] as u32) << 16
        | (bytes[offset + 3] as u32) << 24
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset] = value as u8;
    bytes[offset + 1] = (value >> 8) as u8;
    bytes[offset + 2] = (value >> 16) as u8;
    bytes[offset + 3] = (value >> 24) as u8;
}

fn hash(key: i32) -> u32 {
    (key as u32).wrapping_mul(2_654_435_761)
}

fn initialize_map(payload: &mut [u8]) {
    payload.fill(0);
    payload[0..4].copy_from_slice(b"RHMP");
    write_u32(payload, 4, 1);
    write_u32(payload, 8, 0);
}

fn validate_map(payload: &[u8]) -> bool {
    if payload.len() != PAYLOAD_SIZE
        || &payload[0..4] != b"RHMP"
        || read_u32(payload, 4) != 1
        || read_u32(payload, 8) > CAPACITY as u32
    {
        return false;
    }
    let mut occupied = 0;
    for index in 0..CAPACITY {
        let marker = payload[HEADER_SIZE + index * SLOT_SIZE];
        if marker > 1 {
            return false;
        }
        occupied += marker as u32;
    }
    occupied == read_u32(payload, 8)
}

fn find_slot(payload: &[u8], key: i32) -> Option<usize> {
    let key_hash = hash(key);
    let start = key_hash as usize % CAPACITY;
    for probe in 0..CAPACITY {
        let index = (start + probe) % CAPACITY;
        let offset = HEADER_SIZE + index * SLOT_SIZE;
        if payload[offset] == 0 {
            return None;
        }
        if read_u32(payload, offset + 1) == key_hash && read_u32(payload, offset + 5) as i32 == key
        {
            return Some(offset);
        }
    }
    None
}

fn find_insert_slot(payload: &[u8], key: i32) -> Option<(usize, bool)> {
    let key_hash = hash(key);
    let start = key_hash as usize % CAPACITY;
    for probe in 0..CAPACITY {
        let index = (start + probe) % CAPACITY;
        let offset = HEADER_SIZE + index * SLOT_SIZE;
        if payload[offset] == 0 {
            return Some((offset, true));
        }
        if read_u32(payload, offset + 1) == key_hash && read_u32(payload, offset + 5) as i32 == key
        {
            return Some((offset, false));
        }
    }
    None
}

#[no_mangle]
pub extern "C" fn redis_abi_version() -> i32 {
    ABI_VERSION
}

#[no_mangle]
pub extern "C" fn redis_init() -> i32 {
    if register_blob_type(TYPE_NAME).is_err() {
        return 1;
    }
    if register_function("rust_map_create", "rust_map_create").is_err() {
        return 2;
    }
    if register_function("rust_map_set", "rust_map_set").is_err() {
        return 3;
    }
    if register_function("rust_map_get", "rust_map_get").is_err() {
        return 4;
    }
    if register_function("rust_map_len", "rust_map_len").is_err() {
        return 5;
    }
    0
}

#[no_mangle]
pub extern "C" fn rust_map_create() -> i32 {
    let mut input_bytes = [0u8; 4096];
    let mut items = [Slice::EMPTY; 8];
    let input = match read_input(&mut input_bytes, &mut items) {
        Ok(value) => value,
        Err(_) => {
            error("invalid Rust function input");
            return 0;
        }
    };
    if input.keys_count() != 1 || input.args_count() != 0 {
        error("rust_map_create expects one key");
        return 0;
    }
    let mut payload = [0u8; PAYLOAD_SIZE];
    initialize_map(&mut payload);
    if blob_write(input.key(0), TYPE_NAME, &payload).is_err() {
        error("failed writing Rust hashmap blob");
        return 0;
    }
    status("Rust hashmap created");
    0
}

#[no_mangle]
pub extern "C" fn rust_map_set() -> i32 {
    let mut input_bytes = [0u8; 4096];
    let mut items = [Slice::EMPTY; 8];
    let input = match read_input(&mut input_bytes, &mut items) {
        Ok(value) => value,
        Err(_) => {
            error("invalid Rust function input");
            return 0;
        }
    };
    if input.keys_count() != 1 || input.args_count() != 2 {
        error("rust_map_set expects one key, field, and value");
        return 0;
    }
    let field = match parse_i32(input.arg(0)) {
        Some(value) => value,
        None => {
            error("hashmap field must be an i32");
            return 0;
        }
    };
    let value = match parse_i32(input.arg(1)) {
        Some(value) => value,
        None => {
            error("hashmap value must be an i32");
            return 0;
        }
    };

    let mut payload = [0u8; PAYLOAD_SIZE];
    let length = match blob_read(input.key(0), TYPE_NAME, &mut payload) {
        Ok(value) => value,
        Err(_) => {
            error("failed reading Rust hashmap blob");
            return 0;
        }
    };
    if !validate_map(&payload[..length]) {
        error("invalid Rust hashmap blob");
        return 0;
    }
    let (offset, inserted) = match find_insert_slot(&payload, field) {
        Some(value) => value,
        None => {
            error("Rust hashmap is full");
            return 0;
        }
    };
    payload[offset] = 1;
    write_u32(&mut payload, offset + 1, hash(field));
    write_u32(&mut payload, offset + 5, field as u32);
    write_u32(&mut payload, offset + 9, value as u32);
    if inserted {
        let count = read_u32(&payload, 8);
        write_u32(&mut payload, 8, count + 1);
    }
    if blob_write(input.key(0), TYPE_NAME, &payload).is_err() {
        error("failed updating Rust hashmap blob");
        return 0;
    }
    status("Rust hashmap updated");
    0
}

#[no_mangle]
pub extern "C" fn rust_map_get() -> i32 {
    let mut input_bytes = [0u8; 4096];
    let mut items = [Slice::EMPTY; 8];
    let input = match read_input(&mut input_bytes, &mut items) {
        Ok(value) => value,
        Err(_) => {
            error("invalid Rust function input");
            return 0;
        }
    };
    if input.keys_count() != 1 || input.args_count() != 1 {
        error("rust_map_get expects one key and one field");
        return 0;
    }
    let field = match parse_i32(input.arg(0)) {
        Some(value) => value,
        None => {
            error("hashmap field must be an i32");
            return 0;
        }
    };
    let mut payload = [0u8; PAYLOAD_SIZE];
    let length = match blob_read(input.key(0), TYPE_NAME, &mut payload) {
        Ok(value) => value,
        Err(_) => {
            error("failed reading Rust hashmap blob");
            return 0;
        }
    };
    if !validate_map(&payload[..length]) {
        error("invalid Rust hashmap blob");
        return 0;
    }
    let offset = match find_slot(&payload, field) {
        Some(value) => value,
        None => {
            error("hashmap field not found");
            return 0;
        }
    };
    let mut reply = [0u8; 32];
    if reply_integer(read_u32(&payload, offset + 9) as i32, &mut reply).is_err() {
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn rust_map_len() -> i32 {
    let mut input_bytes = [0u8; 4096];
    let mut items = [Slice::EMPTY; 8];
    let input = match read_input(&mut input_bytes, &mut items) {
        Ok(value) => value,
        Err(_) => {
            error("invalid Rust function input");
            return 0;
        }
    };
    if input.keys_count() != 1 || input.args_count() != 0 {
        error("rust_map_len expects one key");
        return 0;
    }
    let mut payload = [0u8; PAYLOAD_SIZE];
    let length = match blob_read(input.key(0), TYPE_NAME, &mut payload) {
        Ok(value) => value,
        Err(_) => {
            error("failed reading Rust hashmap blob");
            return 0;
        }
    };
    if !validate_map(&payload[..length]) {
        error("invalid Rust hashmap blob");
        return 0;
    }
    let mut reply = [0u8; 32];
    if reply_integer(read_u32(&payload, 8) as i32, &mut reply).is_err() {
        return 1;
    }
    0
}
