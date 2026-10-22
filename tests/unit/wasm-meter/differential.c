/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
/* Independent original-versus-injected execution evidence. Each finite case
 * runs in fresh stock FastJIT instances with identical typed arguments. Compare
 * results, all default linear memory, original exported globals, ordered native
 * observations and ordinary traps. Private fuel, timing, resource-limit
 * equivalence and unexported globals are outside this finite comparison. */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_export.h"
#include "wasm_meter.h"

static unsigned cases, checks, trapped, host_cases;
static const char *module_name, *function_name;
static unsigned variant;
static void require(int ok, const char *why) {
    if (!ok) { fprintf(stderr,"FAIL %s/%s variant=%u: %s\n",module_name,function_name,variant,why); exit(1); }
    checks++;
}
typedef struct { uint32_t which, argument, result, progress; uint8_t memory[16]; } Event;
static Event trace[128];
static unsigned trace_count;
static int32_t host(wasm_exec_env_t env, int32_t argument, unsigned which) {
    require(trace_count < 128, "trace capacity");
    Event *e = &trace[trace_count++]; memset(e,0,sizeof(*e));
    e->which=which; e->argument=(uint32_t)argument; e->result=(uint32_t)argument*2;
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);
    wasm_global_inst_t global;
    if(wasm_runtime_get_export_global_inst(inst,"progress",&global)) memcpy(&e->progress,global.global_data,4);
    wasm_memory_inst_t memory=wasm_runtime_get_default_memory(inst);
    if(memory && wasm_memory_get_cur_page_count(memory)) memcpy(e->memory,wasm_memory_get_base_address(memory),16);
    if(which && wasm_runtime_get_export_global_inst(inst,WASM_METER_FUEL,&global)) {
        uint64_t fuel; memcpy(&fuel,global.global_data,8); fuel-=3; memcpy(global.global_data,&fuel,8);
    }
    return (int32_t)e->result;
}
static int32_t observe(wasm_exec_env_t env,int32_t x) { return host(env,x,0); }
static int32_t spend(wasm_exec_env_t env,int32_t x) { return host(env,x,1); }
static NativeSymbol imports[]={{"observe",observe,"(i)i",NULL},{"spend",spend,"(i)i",NULL}};
static uint8_t *read_bytes(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb"); require(f!=NULL,"open fixture");
    require(!fseek(f,0,SEEK_END),"seek"); long size=ftell(f); require(size>=0,"file size"); rewind(f);
    uint8_t *b=malloc((size_t)size); require(b!=NULL,"allocate bytes");
    require(fread(b,1,(size_t)size,f)==(size_t)size,"read"); fclose(f); *n=(size_t)size; return b;
}
static int finite_case(const char *name,unsigned v) {
    if(!strncmp(name,"spin",4)||!strncmp(name,"cycle",5)||!strcmp(name,"effect_backedge")||!strcmp(name,"loop_tree")) return 0;
    if(!strcmp(name,"mixed_exit") && v!=1) return 0;
    if((!strcmp(name,"counted_if")||!strcmp(name,"counted_table")) && v==0) return 0;
    return 1;
}
static void compare_value(wasm_val_t a,wasm_val_t b) {
    require(a.kind==b.kind,"result type");
    if(a.kind==WASM_F32 && isnan(a.of.f32) && isnan(b.of.f32)) return;
    if(a.kind==WASM_F64 && isnan(a.of.f64) && isnan(b.of.f64)) return;
    unsigned n=a.kind==WASM_I32||a.kind==WASM_F32?4:8;
    require(!memcmp(&a.of,&b.of,n),"result bits");
}
static void compare_state(wasm_module_t original,wasm_module_inst_t a,wasm_module_inst_t b) {
    wasm_memory_inst_t ma=wasm_runtime_get_default_memory(a),mb=wasm_runtime_get_default_memory(b);
    require((ma!=NULL)==(mb!=NULL),"memory presence");
    if(ma) {
        uint64_t na=wasm_memory_get_cur_page_count(ma)*wasm_memory_get_bytes_per_page(ma);
        uint64_t nb=wasm_memory_get_cur_page_count(mb)*wasm_memory_get_bytes_per_page(mb);
        require(na==nb,"memory size");
        require(!na || !memcmp(wasm_memory_get_base_address(ma),wasm_memory_get_base_address(mb),(size_t)na),"all linear-memory bytes");
    }
    int count=wasm_runtime_get_export_count(original);
    for(int i=0;i<count;i++) {
        wasm_export_t item; wasm_runtime_get_export_type(original,i,&item);
        if(item.kind!=WASM_IMPORT_EXPORT_KIND_GLOBAL) continue;
        wasm_global_inst_t ga,gb;
        require(wasm_runtime_get_export_global_inst(a,item.name,&ga)&&wasm_runtime_get_export_global_inst(b,item.name,&gb),"original global present");
        wasm_valkind_t kind=wasm_global_type_get_valkind(item.u.global_type);
        require(!memcmp(ga.global_data,gb.global_data,kind==WASM_I32||kind==WASM_F32?4:8),"original exported global bits");
    }
}
static void compare_case(wasm_module_t original,wasm_module_t injected,const char *name,unsigned v) {
    function_name=name; variant=v; char error[512]={0};
    wasm_module_inst_t a=wasm_runtime_instantiate(original,65536,0,error,sizeof(error));
    require(a!=NULL,error);
    wasm_module_inst_t b=wasm_runtime_instantiate(injected,65536,0,error,sizeof(error));
    require(b!=NULL,error);
    compare_state(original,a,b);
    wasm_exec_env_t ea=wasm_runtime_create_exec_env(a,65536),eb=wasm_runtime_create_exec_env(b,65536);
    require(ea&&eb,"execution environments");
    wasm_function_inst_t fa=wasm_runtime_lookup_function(a,name),fb=wasm_runtime_lookup_function(b,name);
    require(fa&&fb,"original export function retained");
    unsigned argc=wasm_func_get_param_count(fa,a),resultc=wasm_func_get_result_count(fa,a);
    require(argc<=8&&resultc<=8,"test argument capacity");
    require(argc==wasm_func_get_param_count(fb,b)&&resultc==wasm_func_get_result_count(fb,b),"original signature retained");
    wasm_valkind_t types[8]; wasm_func_get_param_types(fa,a,types);
    wasm_val_t args[8]={{0}},ra[8]={{0}},rb[8]={{0}};
    const int values[]={0,1,2,7,-1};
    const uint32_t f32[]={0,0x80000000,0x3fa00000,0x7fc01234,0x7f800000};
    const uint64_t f64[]={0,UINT64_C(0x8000000000000000),UINT64_C(0xc004000000000000),UINT64_C(0x7ff8000000001234),UINT64_C(0x7ff0000000000000)};
    for(unsigned i=0;i<argc;i++) {
        args[i].kind=types[i];
        if(types[i]==WASM_I32) args[i].of.i32=values[v];
        else if(types[i]==WASM_I64) args[i].of.i64=INT64_C(876543210123)+values[v];
        else if(types[i]==WASM_F32) memcpy(&args[i].of.f32,&f32[v],4);
        else if(types[i]==WASM_F64) memcpy(&args[i].of.f64,&f64[v],8);
        else require(0,"supported numeric argument");
    }
    trace_count=0; memset(trace,0,sizeof(trace));
    int oka=wasm_runtime_call_wasm_a(ea,fa,resultc,ra,argc,args);
    Event ta[128]; unsigned na=trace_count; memcpy(ta,trace,sizeof(ta));
    char trap_a[512]={0}; const char *ex=wasm_runtime_get_exception(a); if(ex) snprintf(trap_a,sizeof(trap_a),"%s",ex);
    trace_count=0; memset(trace,0,sizeof(trace));
    int okb=wasm_runtime_call_wasm_a(eb,fb,resultc,rb,argc,args);
    require(oka==okb,"success/trap outcome");
    require(na==trace_count&&!memcmp(ta,trace,na*sizeof(Event)),"ordered host trace incl observed memory/global state");
    if(na) host_cases++;
    if(oka) for(unsigned i=0;i<resultc;i++) compare_value(ra[i],rb[i]);
    else {
        trapped++; const char *trap_b=wasm_runtime_get_exception(b);
        if(!trap_b || strcmp(trap_a,trap_b)) fprintf(stderr,"original trap=%s injected trap=%s\n",trap_a,trap_b?trap_b:"NULL");
        require(trap_b&&!strcmp(trap_a,trap_b),"ordinary trap text/category");
    }
    wasm_global_inst_t fuel_flag;
    require(wasm_runtime_get_export_global_inst(b,WASM_METER_EXHAUSTED,&fuel_flag),"meter marker");
    int32_t flag; memcpy(&flag,fuel_flag.global_data,4); require(!flag,"ample fuel must not exhaust");
    compare_state(original,a,b); cases++;
    printf("PASS %s/%s variant=%u outcome=%s host_events=%u\n",module_name,name,v,oka?"return":"trap",na);
    wasm_runtime_destroy_exec_env(ea); wasm_runtime_destroy_exec_env(eb);
    wasm_runtime_deinstantiate(a); wasm_runtime_deinstantiate(b);
}
int main(int argc,char **argv) {
    require(argc==2,"fixture directory");
    RuntimeInitArgs init={0}; init.running_mode=Mode_Fast_JIT; init.mem_alloc_type=Alloc_With_System_Allocator;
    init.native_module_name="env"; init.native_symbols=imports; init.n_native_symbols=2;
    require(wasm_runtime_full_init(&init),"runtime init");
    const char *modules[]={"fixtures","checkpoints","leaf-calls","call-trees","prefix","leaf-start","tree-start","start-finite","tree-types","wide-indices","numerical"};
    for(unsigned k=0;k<sizeof(modules)/sizeof(*modules);k++) {
        module_name=modules[k]; function_name="load"; variant=0;
        char path[2048],error[512]={0}; snprintf(path,sizeof(path),"%s/%s.wasm",argv[1],module_name);
        size_t length,outlen; uint8_t *bytes=read_bytes(path,&length),*validation=malloc(length),*out=NULL;
        require(validation!=NULL,"original bytes copy"); memcpy(validation,bytes,length);
        wasm_module_t original=wasm_runtime_load(validation,(uint32_t)length,error,sizeof(error)); require(original!=NULL,error);
        require(wasmMeterInject(bytes,length,INT64_MAX,&out,&outlen,error,sizeof(error)),error);
        wasm_module_t injected=wasm_runtime_load(out,(uint32_t)outlen,error,sizeof(error)); require(injected!=NULL,error);
        int count=wasm_runtime_get_export_count(original);
        for(int i=0;i<count;i++) {
            wasm_export_t item; wasm_runtime_get_export_type(original,i,&item);
            if(item.kind!=WASM_IMPORT_EXPORT_KIND_FUNC) continue;
            for(unsigned v=0;v<(!strcmp(module_name,"numerical")?5U:4U);v++)
                if(finite_case(item.name,v)) compare_case(original,injected,item.name,v);
        }
        wasm_runtime_unload(original); wasm_runtime_unload(injected); free(bytes); free(validation); free(out);
    }
    wasm_runtime_destroy();
    printf("SUMMARY cases=%u assertions=%u ordinary_trap_cases=%u host_trace_cases=%u\n",cases,checks,trapped,host_cases);
    return 0;
}
