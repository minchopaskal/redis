package main

import "github.com/redis/redis/wasm-go/rediswasm"

var binaryTreeType rediswasm.BlobType

type treeNode struct {
	Value int32
	Left  *treeNode
	Right *treeNode
}

//export redis_abi_version
func redisABIVersion() int32 {
	return rediswasm.ABIVersion
}

//export redis_init
func redisInit() int32 {
	var err error
	binaryTreeType, err = rediswasm.RegisterBlobType("binary_tree")
	if err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("go_set", "go_set"); err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("go_get", "go_get"); err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("tree_create", "tree_create"); err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("tree_insert", "tree_insert"); err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("tree_contains", "tree_contains"); err != nil {
		return 1
	}
	return 0
}

//export go_set
func goSet() int32 {
	keys, args, err := rediswasm.Input()
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if len(keys) != 1 || len(args) != 1 {
		rediswasm.ReplyError("go_set expects one key and one value")
		return 0
	}
	if _, err := rediswasm.Call([]byte("SET"), keys[0], args[0]); err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	rediswasm.Status("stored by Go")
	return 0
}

//export go_get
func goGet() int32 {
	keys, args, err := rediswasm.Input()
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if len(keys) != 1 || len(args) != 0 {
		rediswasm.ReplyError("go_get expects one key")
		return 0
	}
	reply, err := rediswasm.Call([]byte("GET"), keys[0])
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if err := rediswasm.ReplyRaw(reply); err != nil {
		rediswasm.ReplyError(err.Error())
	}
	return 0
}

func parseInt32(value []byte) (int32, bool) {
	if len(value) == 0 {
		return 0, false
	}
	negative := false
	cursor := 0
	if value[0] == '-' {
		negative = true
		cursor++
	}
	if cursor == len(value) {
		return 0, false
	}
	var result int64
	for ; cursor < len(value); cursor++ {
		if value[cursor] < '0' || value[cursor] > '9' {
			return 0, false
		}
		result = result*10 + int64(value[cursor]-'0')
		limit := int64(2147483647)
		if negative {
			limit++
		}
		if result > limit {
			return 0, false
		}
	}
	if negative {
		result = -result
	}
	return int32(result), true
}

func insertNode(root **treeNode, value int32) {
	for *root != nil {
		if value < (*root).Value {
			root = &(*root).Left
		} else if value > (*root).Value {
			root = &(*root).Right
		} else {
			return
		}
	}
	*root = &treeNode{Value: value}
}

func appendInt32LE(dst []byte, value int32) []byte {
	u := uint32(value)
	return append(dst, byte(u), byte(u>>8), byte(u>>16), byte(u>>24))
}

// Trees use a pre-order encoding: 0 for nil, or 1 + little-endian int32 value
// followed recursively by the left and right children.
func encodeTree(dst []byte, node *treeNode) []byte {
	if node == nil {
		return append(dst, 0)
	}
	dst = append(dst, 1)
	dst = appendInt32LE(dst, node.Value)
	dst = encodeTree(dst, node.Left)
	return encodeTree(dst, node.Right)
}

func decodeTree(src []byte, cursor *int, depth int) (*treeNode, bool) {
	if depth > 1024 || *cursor >= len(src) {
		return nil, false
	}
	marker := src[*cursor]
	*cursor++
	if marker == 0 {
		return nil, true
	}
	if marker != 1 || len(src)-*cursor < 4 {
		return nil, false
	}
	p := src[*cursor : *cursor+4]
	value := int32(uint32(p[0]) | uint32(p[1])<<8 | uint32(p[2])<<16 | uint32(p[3])<<24)
	*cursor += 4
	left, ok := decodeTree(src, cursor, depth+1)
	if !ok {
		return nil, false
	}
	right, ok := decodeTree(src, cursor, depth+1)
	if !ok {
		return nil, false
	}
	return &treeNode{Value: value, Left: left, Right: right}, true
}

func readTree(key []byte) (*treeNode, error) {
	payload, err := rediswasm.BlobRead(key, binaryTreeType)
	if err != nil {
		return nil, err
	}
	cursor := 0
	root, ok := decodeTree(payload, &cursor, 0)
	if !ok || cursor != len(payload) {
		return nil, rediswasm.Error("invalid binary tree payload")
	}
	return root, nil
}

func replyInteger(value bool) {
	if value {
		_ = rediswasm.ReplyRaw([]byte(":1\r\n"))
	} else {
		_ = rediswasm.ReplyRaw([]byte(":0\r\n"))
	}
}

//export tree_create
func treeCreate() int32 {
	keys, args, err := rediswasm.Input()
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if len(keys) != 1 || len(args) == 0 {
		rediswasm.ReplyError("tree_create expects one key and one or more integers")
		return 0
	}
	var root *treeNode
	for _, arg := range args {
		value, ok := parseInt32(arg)
		if !ok {
			rediswasm.ReplyError("tree values must be signed 32-bit integers")
			return 0
		}
		insertNode(&root, value)
	}
	if err := rediswasm.BlobWrite(keys[0], binaryTreeType, encodeTree(nil, root)); err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	rediswasm.Status("tree created")
	return 0
}

//export tree_insert
func treeInsert() int32 {
	keys, args, err := rediswasm.Input()
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if len(keys) != 1 || len(args) != 1 {
		rediswasm.ReplyError("tree_insert expects one key and one integer")
		return 0
	}
	value, ok := parseInt32(args[0])
	if !ok {
		rediswasm.ReplyError("tree value must be a signed 32-bit integer")
		return 0
	}
	root, err := readTree(keys[0])
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	insertNode(&root, value)
	if err := rediswasm.BlobWrite(keys[0], binaryTreeType, encodeTree(nil, root)); err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	rediswasm.Status("tree updated")
	return 0
}

//export tree_contains
func treeContains() int32 {
	keys, args, err := rediswasm.Input()
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	if len(keys) != 1 || len(args) != 1 {
		rediswasm.ReplyError("tree_contains expects one key and one integer")
		return 0
	}
	value, ok := parseInt32(args[0])
	if !ok {
		rediswasm.ReplyError("tree value must be a signed 32-bit integer")
		return 0
	}
	root, err := readTree(keys[0])
	if err != nil {
		rediswasm.ReplyError(err.Error())
		return 0
	}
	for root != nil {
		if value < root.Value {
			root = root.Left
		} else if value > root.Value {
			root = root.Right
		} else {
			replyInteger(true)
			return 0
		}
	}
	replyInteger(false)
	return 0
}

func main() {}
