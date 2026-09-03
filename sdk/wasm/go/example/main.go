package main

import "github.com/redis/redis/wasm-go/rediswasm"

//export redis_abi_version
func redisABIVersion() int32 {
	return rediswasm.ABIVersion
}

//export redis_init
func redisInit() int32 {
	if err := rediswasm.RegisterFunction("go_set", "go_set"); err != nil {
		return 1
	}
	if err := rediswasm.RegisterFunction("go_get", "go_get"); err != nil {
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

func main() {}
