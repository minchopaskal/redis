// Package rediswasm implements the guest side of the Redis WASM Functions
// proof-of-concept ABI. It is intended for TinyGo's wasm-unknown target.
package rediswasm

import "unsafe"

const ABIVersion int32 = 1

type Error string

func (e Error) Error() string { return string(e) }

const (
	ErrHost           = Error("redis wasm host call failed")
	ErrInvalidInput   = Error("invalid packed function input")
	ErrInvalidReply   = Error("invalid RESP reply")
	ErrBufferTooLarge = Error("buffer is too large")
)

// RESPEntry is one flat, depth-first token produced by the host RESP parser.
type RESPEntry struct {
	Tag        byte
	Offset     uint32
	Length     uint32
	ChildCount uint32
}

//go:wasmimport redis input_len
func hostInputLen() int32

//go:wasmimport redis input_read
func hostInputRead(dst uint32, cap int32) int32

//go:wasmimport redis keys_count
func hostKeysCount() int32

//go:wasmimport redis call
func hostCall(ptr uint32, length int32) int32

//go:wasmimport redis reply_read
func hostReplyRead(dst uint32, cap int32, offset int32) int32

//go:wasmimport redis last_error_read
func hostLastErrorRead(dst uint32, cap int32) int32

//go:wasmimport redis parse_resp
func hostParseRESP(src uint32, srcLen int32, out uint32, outCap int32) int32

//go:wasmimport redis reply_raw
func hostReplyRaw(ptr uint32, length int32) int32

//go:wasmimport redis status_reply
func hostStatusReply(ptr uint32, length int32)

//go:wasmimport redis error_reply
func hostErrorReply(ptr uint32, length int32)

//go:wasmimport redis register_function
func hostRegisterFunction(name uint32, nameLen int32, export uint32, exportLen int32) int32

func bytesPointer(value []byte) uint32 {
	if len(value) == 0 {
		return 0
	}
	return uint32(uintptr(unsafe.Pointer(&value[0])))
}

func putUint32(dst []byte, value uint32) {
	dst[0] = byte(value)
	dst[1] = byte(value >> 8)
	dst[2] = byte(value >> 16)
	dst[3] = byte(value >> 24)
}

func uint32At(src []byte) uint32 {
	return uint32(src[0]) |
		uint32(src[1])<<8 |
		uint32(src[2])<<16 |
		uint32(src[3])<<24
}

func checkedLength(length int) (int32, error) {
	if uint64(length) > uint64(^uint32(0)>>1) {
		return 0, ErrBufferTooLarge
	}
	return int32(length), nil
}

type hostBufferKind byte

const (
	hostInputBuffer hostBufferKind = iota
	hostReplyBuffer
	hostErrorBuffer
)

func readHostBuffer(length int32, kind hostBufferKind) ([]byte, error) {
	if length < 0 {
		return nil, ErrHost
	}
	if length == 0 {
		return []byte{}, nil
	}
	buffer := make([]byte, int(length))
	var written int32
	switch kind {
	case hostInputBuffer:
		written = hostInputRead(bytesPointer(buffer), length)
	case hostReplyBuffer:
		written = hostReplyRead(bytesPointer(buffer), length, 0)
	case hostErrorBuffer:
		written = hostLastErrorRead(bytesPointer(buffer), length)
	}
	if written != length {
		return nil, ErrHost
	}
	return buffer, nil
}

// Input returns the binary-safe keys and arguments supplied to FCALL.
func Input() (keys [][]byte, args [][]byte, err error) {
	length := hostInputLen()
	packed, err := readHostBuffer(length, hostInputBuffer)
	if err != nil {
		return nil, nil, err
	}
	if len(packed) < 4 {
		return nil, nil, ErrInvalidInput
	}

	count := int(uint32At(packed))
	keyCount := int(hostKeysCount())
	if keyCount < 0 || keyCount > count {
		return nil, nil, ErrInvalidInput
	}
	items := make([][]byte, 0, count)
	cursor := 4
	for i := 0; i < count; i++ {
		if cursor+4 > len(packed) {
			return nil, nil, ErrInvalidInput
		}
		itemLength := int(uint32At(packed[cursor : cursor+4]))
		cursor += 4
		if itemLength < 0 || itemLength > len(packed)-cursor {
			return nil, nil, ErrInvalidInput
		}
		items = append(items, packed[cursor:cursor+itemLength])
		cursor += itemLength
	}
	if cursor != len(packed) {
		return nil, nil, ErrInvalidInput
	}
	return items[:keyCount], items[keyCount:], nil
}

func packCommand(argv [][]byte) ([]byte, error) {
	if len(argv) == 0 {
		return nil, ErrInvalidInput
	}
	total := uint64(4)
	for _, arg := range argv {
		total += 4 + uint64(len(arg))
	}
	if total > uint64(^uint32(0)>>1) {
		return nil, ErrBufferTooLarge
	}

	packed := make([]byte, int(total))
	putUint32(packed, uint32(len(argv)))
	cursor := 4
	for _, arg := range argv {
		putUint32(packed[cursor:cursor+4], uint32(len(arg)))
		cursor += 4
		copy(packed[cursor:], arg)
		cursor += len(arg)
	}
	return packed, nil
}

// Call runs one Redis command and returns its staged RESP3 reply. Redis command
// errors are returned as valid RESP and can be forwarded or parsed by the guest.
func Call(argv ...[]byte) ([]byte, error) {
	packed, err := packCommand(argv)
	if err != nil {
		return nil, err
	}
	length, err := checkedLength(len(packed))
	if err != nil {
		return nil, err
	}
	replyLength := hostCall(bytesPointer(packed), length)
	if replyLength < 0 {
		return nil, Error(LastError())
	}
	return readHostBuffer(replyLength, hostReplyBuffer)
}

// LastError returns the most recent host rejection message.
func LastError() string {
	required := hostLastErrorRead(0, 0)
	if required >= 0 {
		return ""
	}
	length := -required
	message, err := readHostBuffer(length, hostErrorBuffer)
	if err != nil {
		return string(ErrHost)
	}
	return string(message)
}

// RegisterFunction registers a no-argument i32 callback during redis_init.
func RegisterFunction(name, exportName string) error {
	nameBytes := []byte(name)
	exportBytes := []byte(exportName)
	nameLength, err := checkedLength(len(nameBytes))
	if err != nil {
		return err
	}
	exportLength, err := checkedLength(len(exportBytes))
	if err != nil {
		return err
	}
	if hostRegisterFunction(bytesPointer(nameBytes), nameLength,
		bytesPointer(exportBytes), exportLength) != 0 {
		return Error(LastError())
	}
	return nil
}

// ReplyRaw forwards one complete, host-validated RESP value to the caller.
func ReplyRaw(reply []byte) error {
	length, err := checkedLength(len(reply))
	if err != nil {
		return err
	}
	if hostReplyRaw(bytesPointer(reply), length) != 0 {
		return ErrInvalidReply
	}
	return nil
}

// Status replies with one RESP simple string.
func Status(message string) {
	value := []byte(message)
	hostStatusReply(bytesPointer(value), int32(len(value)))
}

// ReplyError replies with one RESP simple error.
func ReplyError(message string) {
	value := []byte(message)
	hostErrorReply(bytesPointer(value), int32(len(value)))
}

// ParseRESP asks Redis to tokenize one complete RESP value.
func ParseRESP(reply []byte) ([]RESPEntry, error) {
	length, err := checkedLength(len(reply))
	if err != nil {
		return nil, err
	}
	required := hostParseRESP(bytesPointer(reply), length, 0, 0)
	if required == -1 || required == 0 || required%16 != 0 {
		return nil, ErrInvalidReply
	}
	if required < 0 {
		required = -required
	}
	raw := make([]byte, int(required))
	written := hostParseRESP(bytesPointer(reply), length, bytesPointer(raw), required)
	if written != required {
		return nil, ErrInvalidReply
	}

	entries := make([]RESPEntry, len(raw)/16)
	for i := range entries {
		entry := raw[i*16 : i*16+16]
		entries[i] = RESPEntry{
			Tag:        byte(uint32At(entry[0:4])),
			Offset:     uint32At(entry[4:8]),
			Length:     uint32At(entry[8:12]),
			ChildCount: uint32At(entry[12:16]),
		}
	}
	return entries, nil
}
