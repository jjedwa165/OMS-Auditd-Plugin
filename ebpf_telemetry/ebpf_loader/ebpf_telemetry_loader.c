/*
    microsoft-oms-auditd-plugin

    Copyright (c) Microsoft Corporation

    All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/


#include "ebpf_telemetry_loader.h"
#include "../event_defs.h"

//Notes:
//https://github.com/vmware/p4c-xdp/issues/58
//https://github.com/libbpf/libbpf/commit/9007494e6c3641e82a3e8176b6e0b0fb0e77f683
//https://elinux.org/images/d/dc/Kernel-Analysis-Using-eBPF-Daniel-Thompson-Linaro.pdf
//https://kinvolk.io/blog/2018/02/timing-issues-when-using-bpf-with-virtual-cpus/
//https://blogs.oracle.com/linux/notes-on-bpf-3
//https://elixir.free-electrons.com/linux/latest/source/samples/bpf/bpf_load.c#L339
//https://stackoverflow.com/questions/57628432/ebpf-maps-for-one-element-map-type-and-kernel-user-space-communication

#define MAP_PAGE_SIZE 1024
#define DEBUGFS "/sys/kernel/debug/tracing/"

#define KERN_TRACEPOINT_OBJ "ebpf_loader/ebpf_telemetry_kern_tp.o"
#define KERN_RAW_TRACEPOINT_OBJ "ebpf_loader/ebpf_telemetry_kern_raw_tp.o"

static int    event_map_fd          = 0;
static int    config_map_fd         = 0;
static struct utsname     uname_s   = { 0 };
static struct bpf_object  *bpf_obj  = NULL;

static struct bpf_program *bpf_sys_enter_openat  = NULL;
static struct bpf_program *bpf_sys_enter_execve  = NULL;
static struct bpf_program *bpf_sys_enter_connect = NULL;
static struct bpf_program *bpf_sys_enter_accept  = NULL;
static struct bpf_program *bpf_sys_exit_accept   = NULL;

static struct bpf_program *bpf_sys_enter = NULL;
static struct bpf_program *bpf_sys_exit  = NULL;

static struct bpf_link    *bpf_sys_enter_openat_link  = NULL;
static struct bpf_link    *bpf_sys_enter_execve_link  = NULL;
static struct bpf_link    *bpf_sys_enter_connect_link = NULL;
static struct bpf_link    *bpf_sys_enter_accept_link  = NULL;
static struct bpf_link    *bpf_sys_exit_accept_link   = NULL;

static struct bpf_link    *bpf_sys_enter_link = NULL;
static struct bpf_link    *bpf_sys_exit_link  = NULL;

typedef enum bpf_type { NOBPF, BPF_TP, BPF_RAW_TP } bpf_type;

static bpf_type support_version = NOBPF;

void ebpf_telemetry_close_all(){
    
    if ( support_version == BPF_TP ){
        bpf_link__destroy(bpf_sys_enter_openat_link);
        bpf_link__destroy(bpf_sys_enter_execve_link);
        bpf_link__destroy(bpf_sys_enter_connect_link);
        bpf_link__destroy(bpf_sys_enter_accept_link);
        bpf_link__destroy(bpf_sys_exit_accept_link);
    }
    else{
        bpf_link__destroy(bpf_sys_enter_link);
        bpf_link__destroy(bpf_sys_exit_link);
    }

    bpf_object__close(bpf_obj);
}

void populate_config_offsets(config_s *c)
{
    c->ppid[0] = 2256; c->ppid[1] = 2244; c->ppid[2] = -1;
    c->auid[0] = 2920; c->auid[1] = -1;
    c->ses[0] = 2924; c->ses[1] = -1;

    c->cred[0] = 2712; c->cred[1] = -1;
    c->cred_uid[0] = 4; c->cred_uid[1] = -1;
    c->cred_gid[0] = 8; c->cred_gid[1] = -1;
    c->cred_euid[0] = 20; c->cred_euid[1] = -1;
    c->cred_suid[0] = 12; c->cred_suid[1] = -1;
    c->cred_fsuid[0] = 28; c->cred_fsuid[1] = -1;
    c->cred_egid[0] = 24; c->cred_egid[1] = -1;
    c->cred_sgid[0] = 16; c->cred_sgid[1] = -1;
    c->cred_fsgid[0] = 32; c->cred_fsgid[1] = -1;

    c->tty[0] = 2816; c->tty[1] = 408; c->tty[2] = 368; c->tty[3] = -1;
    c->comm[0] = 2728; c->comm[1] = -1;
    c->exe_dentry[0] = 2064; c->exe_dentry[1] = 928; c->exe_dentry[2] = -1;
    c->dentry_parent = 24;
    c->dentry_name = 40;
}


int ebpf_telemetry_start(void (*event_cb)(void *ctx, int cpu, void *data, __u32 size), void (*events_lost_cb)(void *ctx, int cpu, __u64 lost_cnt))
{
    unsigned int major = 0, minor = 0;
    
    if ( uname(&uname_s) ){
        fprintf(stderr, "Couldn't find uname, '%s'\n", strerror(errno));
        return 1;
    }

    if ( 2 == sscanf(uname_s.release, "%u.%u", &major, &minor)){
        fprintf(stderr, "Found Kernel version: %u.%u\n", major, minor);
    }
    else{
        fprintf(stderr, "Couldn't find version\n");
        return 1;
    }    

    // <  4.12, no ebpf
    // >= 4.12, tracepoints
    // >= 4.17, raw_tracepoints
    if ( ( major <= 4 ) && ( minor < 12 ) ){
        support_version = NOBPF;
        fprintf(stderr, "Kernel Version %u.%u not supported\n", major, minor);
        return 1;    
    }
    else if ( ( major == 4 ) && ( minor < 17 ) ){
        fprintf(stderr, "Using Tracepoints\n");
        support_version = BPF_TP;   
    }
    else if ( ( ( major == 4 ) && ( minor >= 17 ) ) ||
              (            major > 4             ) ){
        fprintf(stderr, "Using Raw Tracepoints\n");
        support_version = BPF_RAW_TP;   
    }

    struct rlimit lim = {
        .rlim_cur = RLIM_INFINITY,
        .rlim_max = RLIM_INFINITY,
    };
    char filename[256];

    if ( support_version == BPF_TP)
        strncpy(filename, KERN_TRACEPOINT_OBJ, sizeof(filename));
    else
        strncpy(filename, KERN_RAW_TRACEPOINT_OBJ, sizeof(filename));
   
    setrlimit(RLIMIT_MEMLOCK, &lim);

    bpf_obj = bpf_object__open(filename);
    if (libbpf_get_error(bpf_obj)) {
        fprintf(stderr, "ERROR: failed to open prog: '%s'\n", strerror(errno));
        return 1;
    }

    if ( ( support_version == BPF_TP ) &&
         ( NULL != ( bpf_sys_enter_openat  = bpf_object__find_program_by_title(bpf_obj,"tracepoint/syscalls/sys_enter_open")))    &&
         ( NULL != ( bpf_sys_enter_execve  = bpf_object__find_program_by_title(bpf_obj,"tracepoint/syscalls/sys_enter_execve")))  && 
         ( NULL != ( bpf_sys_enter_connect = bpf_object__find_program_by_title(bpf_obj,"tracepoint/syscalls/sys_enter_connect"))) &&
         ( NULL != ( bpf_sys_enter_accept  = bpf_object__find_program_by_title(bpf_obj,"tracepoint/syscalls/sys_enter_accept")))  &&
         ( NULL != ( bpf_sys_exit_accept   = bpf_object__find_program_by_title(bpf_obj,"tracepoint/syscalls/sys_exit_accept")))   )
    {
        bpf_program__set_type(bpf_sys_enter_openat, BPF_PROG_TYPE_TRACEPOINT);
        bpf_program__set_type(bpf_sys_enter_execve, BPF_PROG_TYPE_TRACEPOINT);
        bpf_program__set_type(bpf_sys_enter_connect, BPF_PROG_TYPE_TRACEPOINT);
        bpf_program__set_type(bpf_sys_enter_accept, BPF_PROG_TYPE_TRACEPOINT);
        bpf_program__set_type(bpf_sys_exit_accept, BPF_PROG_TYPE_TRACEPOINT);
    }
    else if ( ( support_version ==  BPF_RAW_TP ) &&
              ( NULL != ( bpf_sys_enter = bpf_object__find_program_by_title(bpf_obj,"raw_tracepoint/sys_enter")))  &&
              ( NULL != ( bpf_sys_exit  = bpf_object__find_program_by_title(bpf_obj,"raw_tracepoint/sys_exit")))   )
    {
        bpf_program__set_type(bpf_sys_enter, BPF_PROG_TYPE_RAW_TRACEPOINT);
        bpf_program__set_type(bpf_sys_exit, BPF_PROG_TYPE_RAW_TRACEPOINT);
    }
    else{
        fprintf(stderr, "ERROR: failed to find program: '%s'\n", strerror(errno));
        return 1;
    }

    if (bpf_object__load(bpf_obj)) {
        fprintf(stderr, "ERROR: failed to load prog: '%s'\n", strerror(errno));
        return 1;
    }

    event_map_fd = bpf_object__find_map_fd_by_name(bpf_obj, "event_map");
    if ( 0 >= event_map_fd){
        fprintf(stderr, "ERROR: failed to load event_map_fd: '%s'\n", strerror(errno));
        return 1;
    }

    config_map_fd = bpf_object__find_map_fd_by_name(bpf_obj, "config_map");
    if ( 0 >= config_map_fd) {
        fprintf(stderr, "ERROR: failed to load config_map_fd: '%s'\n", strerror(errno));
        return 1;
    }

    //update the config with the userland pid
    unsigned int config_entry = 0;
    config_s config;
    config.userland_pid = getpid();
    populate_config_offsets(&config);
    if (bpf_map_update_elem(config_map_fd, &config_entry, &config, BPF_ANY)) {
        fprintf(stderr, "ERROR: failed to set config: '%s'\n", strerror(errno));
        return 1;
    }
    
    if ( support_version == BPF_TP ){

        bpf_sys_enter_openat_link  = bpf_program__attach_tracepoint(bpf_sys_enter_openat, "syscalls", "sys_enter_open");
        bpf_sys_enter_execve_link  = bpf_program__attach_tracepoint(bpf_sys_enter_execve, "syscalls", "sys_enter_execve");
        bpf_sys_enter_connect_link = bpf_program__attach_tracepoint(bpf_sys_enter_connect,"syscalls", "sys_enter_connect");
        bpf_sys_enter_accept_link  = bpf_program__attach_tracepoint(bpf_sys_enter_accept, "syscalls", "sys_enter_accept");
        bpf_sys_exit_accept_link   = bpf_program__attach_tracepoint(bpf_sys_exit_accept,  "syscalls", "sys_exit_accept");

        if ( (libbpf_get_error(bpf_sys_enter_openat_link)) || 
             (libbpf_get_error(bpf_sys_enter_execve_link)) ||
             (libbpf_get_error(bpf_sys_enter_connect_link))||
             (libbpf_get_error(bpf_sys_enter_accept_link)) ||
             (libbpf_get_error(bpf_sys_exit_accept_link))  )
                return 2;
    }
    else{
         
        bpf_sys_enter_link = bpf_program__attach_raw_tracepoint(bpf_sys_enter, "sys_enter");
        bpf_sys_exit_link = bpf_program__attach_raw_tracepoint(bpf_sys_exit, "sys_exit");
        
        if ( (libbpf_get_error(bpf_sys_enter_link)) || 
             (libbpf_get_error(bpf_sys_exit_link))  )
        return 2;
    }

    // from Kernel 5.7.1 ex: trace_output_user.c 
    struct perf_buffer_opts pb_opts = {};
    struct perf_buffer *pb;
    int ret;

    pb_opts.sample_cb = event_cb;
    pb_opts.lost_cb = events_lost_cb;
    pb_opts.ctx     = NULL;
    pb = perf_buffer__new(event_map_fd, MAP_PAGE_SIZE, &pb_opts); // param 2 is page_cnt == number of pages to mmap.
    ret = libbpf_get_error(pb);
    if (ret) {
        fprintf(stderr, "ERROR: failed to setup perf_buffer: %d\n", ret);
        return 1;
    }

    fprintf(stderr, "Running...\n");

    int i = 0;
    while ((ret = perf_buffer__poll(pb, 1000)) >= 0 ) {
        if (i++ > 10) break;
    }

    ebpf_telemetry_close_all();

    return 0;
}

