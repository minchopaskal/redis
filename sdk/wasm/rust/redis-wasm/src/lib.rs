#![no_std]

pub const ABI_VERSION: i32 = 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Host,
    BufferTooSmall,
    InvalidInput,
}

#[derive(Clone, Copy)]
pub struct Slice {
    pub ptr: *const u8,
    pub len: usize,
}

impl Slice {
    pub const EMPTY: Self = Self {
        ptr: core::ptr::null(),
        len: 0,
    };

    pub fn as_bytes(&self) -> &[u8] {
        if self.len == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(self.ptr, self.len) }
    }
}

pub struct Input<'a> {
    items: &'a [Slice],
    keys: usize,
}

impl<'a> Input<'a> {
    pub fn count(&self) -> usize {
        self.items.len()
    }

    pub fn keys_count(&self) -> usize {
        self.keys
    }

    pub fn args_count(&self) -> usize {
        self.items.len() - self.keys
    }

    pub fn key(&self, index: usize) -> Slice {
        self.items[index]
    }

    pub fn arg(&self, index: usize) -> Slice {
        self.items[self.keys + index]
    }
}

#[link(wasm_import_module = "redis")]
extern "C" {
    #[link_name = "input_len"]
    fn host_input_len() -> i32;
    #[link_name = "input_read"]
    fn host_input_read(dst: *mut u8, cap: i32) -> i32;
    #[link_name = "keys_count"]
    fn host_keys_count() -> i32;
    #[link_name = "register_function"]
    fn host_register_function(
        name: *const u8,
        name_len: i32,
        export_name: *const u8,
        export_len: i32,
    ) -> i32;
    #[link_name = "blob_register"]
    fn host_blob_register(type_name: *const u8, type_len: i32) -> i32;
    #[link_name = "blob_len"]
    fn host_blob_len(key: *const u8, key_len: i32, type_name: *const u8, type_len: i32) -> i32;
    #[link_name = "blob_read"]
    fn host_blob_read(
        key: *const u8,
        key_len: i32,
        type_name: *const u8,
        type_len: i32,
        dst: *mut u8,
        cap: i32,
        offset: i32,
    ) -> i32;
    #[link_name = "blob_write"]
    fn host_blob_write(
        key: *const u8,
        key_len: i32,
        type_name: *const u8,
        type_len: i32,
        src: *const u8,
        src_len: i32,
    ) -> i32;
    #[link_name = "status_reply"]
    fn host_status_reply(message: *const u8, len: i32);
    #[link_name = "error_reply"]
    fn host_error_reply(message: *const u8, len: i32);
    #[link_name = "reply_raw"]
    fn host_reply_raw(reply: *const u8, len: i32) -> i32;
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    let value = bytes.get(offset..offset + 4)?;
    Some(
        value[0] as u32
            | (value[1] as u32) << 8
            | (value[2] as u32) << 16
            | (value[3] as u32) << 24,
    )
}

pub fn read_input<'a>(buffer: &'a mut [u8], items: &'a mut [Slice]) -> Result<Input<'a>, Error> {
    let length = unsafe { host_input_len() };
    let keys = unsafe { host_keys_count() };
    if length < 4 || keys < 0 || length as usize > buffer.len() {
        return Err(Error::BufferTooSmall);
    }
    if unsafe { host_input_read(buffer.as_mut_ptr(), length) } != length {
        return Err(Error::Host);
    }
    let bytes = &buffer[..length as usize];
    let count = read_u32(bytes, 0).ok_or(Error::InvalidInput)? as usize;
    if count > items.len() || keys as usize > count {
        return Err(Error::BufferTooSmall);
    }

    let mut cursor = 4;
    for item in items.iter_mut().take(count) {
        let item_len = read_u32(bytes, cursor).ok_or(Error::InvalidInput)? as usize;
        cursor += 4;
        if item_len > bytes.len().saturating_sub(cursor) {
            return Err(Error::InvalidInput);
        }
        *item = Slice {
            ptr: bytes[cursor..].as_ptr(),
            len: item_len,
        };
        cursor += item_len;
    }
    if cursor != bytes.len() {
        return Err(Error::InvalidInput);
    }
    Ok(Input {
        items: &items[..count],
        keys: keys as usize,
    })
}

pub fn register_function(name: &str, export_name: &str) -> Result<(), Error> {
    let result = unsafe {
        host_register_function(
            name.as_ptr(),
            name.len() as i32,
            export_name.as_ptr(),
            export_name.len() as i32,
        )
    };
    if result == 0 {
        Ok(())
    } else {
        Err(Error::Host)
    }
}

pub fn register_blob_type(name: &str) -> Result<(), Error> {
    if unsafe { host_blob_register(name.as_ptr(), name.len() as i32) } == 0 {
        Ok(())
    } else {
        Err(Error::Host)
    }
}

pub fn blob_read(key: Slice, blob_type: &str, dst: &mut [u8]) -> Result<usize, Error> {
    let length = unsafe {
        host_blob_len(
            key.ptr,
            key.len as i32,
            blob_type.as_ptr(),
            blob_type.len() as i32,
        )
    };
    if length < 0 {
        return Err(Error::Host);
    }
    if length as usize > dst.len() {
        return Err(Error::BufferTooSmall);
    }
    let written = unsafe {
        host_blob_read(
            key.ptr,
            key.len as i32,
            blob_type.as_ptr(),
            blob_type.len() as i32,
            dst.as_mut_ptr(),
            length,
            0,
        )
    };
    if written == length {
        Ok(written as usize)
    } else {
        Err(Error::Host)
    }
}

pub fn blob_write(key: Slice, blob_type: &str, payload: &[u8]) -> Result<(), Error> {
    let result = unsafe {
        host_blob_write(
            key.ptr,
            key.len as i32,
            blob_type.as_ptr(),
            blob_type.len() as i32,
            payload.as_ptr(),
            payload.len() as i32,
        )
    };
    if result == 0 {
        Ok(())
    } else {
        Err(Error::Host)
    }
}

pub fn status(message: &str) {
    unsafe { host_status_reply(message.as_ptr(), message.len() as i32) }
}

pub fn error(message: &str) {
    unsafe { host_error_reply(message.as_ptr(), message.len() as i32) }
}

pub fn reply_integer(value: i32, scratch: &mut [u8]) -> Result<(), Error> {
    if scratch.len() < 16 {
        return Err(Error::BufferTooSmall);
    }
    scratch[0] = b':';
    let mut cursor = 1;
    let mut magnitude = if value < 0 {
        scratch[cursor] = b'-';
        cursor += 1;
        -(value as i64)
    } else {
        value as i64
    };
    let mut digits = [0u8; 10];
    let mut count = 0;
    loop {
        digits[count] = b'0' + (magnitude % 10) as u8;
        count += 1;
        magnitude /= 10;
        if magnitude == 0 {
            break;
        }
    }
    while count > 0 {
        count -= 1;
        scratch[cursor] = digits[count];
        cursor += 1;
    }
    scratch[cursor] = b'\r';
    scratch[cursor + 1] = b'\n';
    cursor += 2;
    if unsafe { host_reply_raw(scratch.as_ptr(), cursor as i32) } == 0 {
        Ok(())
    } else {
        Err(Error::Host)
    }
}

pub fn parse_i32(value: Slice) -> Option<i32> {
    let bytes = value.as_bytes();
    if bytes.is_empty() {
        return None;
    }
    let mut cursor = 0;
    let negative = bytes[0] == b'-';
    if negative {
        cursor += 1;
    }
    if cursor == bytes.len() {
        return None;
    }
    let mut result: i64 = 0;
    while cursor < bytes.len() {
        let byte = bytes[cursor];
        if !byte.is_ascii_digit() {
            return None;
        }
        result = result * 10 + (byte - b'0') as i64;
        let limit = if negative {
            2_147_483_648
        } else {
            2_147_483_647
        };
        if result > limit {
            return None;
        }
        cursor += 1;
    }
    Some(if negative {
        (-result) as i32
    } else {
        result as i32
    })
}
