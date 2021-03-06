#include "vmlinux.h"

#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>

#define TASK_COMM_LEN 32
#define TASK_RUNNING 0

const volatile u32 pid_self = 0;
const volatile bool only_trace_container = false;
const volatile int trace_syscall = -1;

struct sys_enter_event_t {
  uid_t uid;
  __u64 cgid;
  __u32 syscall_nr;
  __u64 ts;
  char comm[TASK_COMM_LEN];
};

struct sys_exit_event_t {
  pid_t pid;
  uid_t uid;
  __u64 cgid;
  u32 syscall_nr;
  __u64 latency;
  char comm[TASK_COMM_LEN];
};

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 10240);
  __type(key, u32);
  __type(value, struct sys_enter_event_t);
} sys_enter_entries SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
  __uint(key_size, sizeof(u32));
  __uint(value_size, sizeof(u32));
} sys_exit_events SEC(".maps");

static __always_inline bool allowed_tracing_syscall(u32 syscall_nr) {
  if (trace_syscall < 0 || syscall_nr == trace_syscall) {
    return true;
  }
  return false;
}

static __always_inline bool is_trace_target(pid_t pid, u32 syscall_nr) {
  struct task_struct *task = (void *)bpf_get_current_task();
  u64 inum;
  inum = BPF_CORE_READ(task, nsproxy, pid_ns_for_children, ns.inum);
  // process is container ??
  if (inum == 0xEFFFFFFCU && only_trace_container)
    return false;

  // Don't trace self
  if (pid == pid_self) {
    return false;
  }

  if (allowed_tracing_syscall(syscall_nr)) {
    return true;
  }

  return false;
}

static __always_inline int
trace_sys_enter(struct trace_event_raw_sys_enter *ctx) {

  struct sys_enter_event_t event = {};
  int ret;
  u32 pid = bpf_get_current_pid_tgid() >> 32;

  if (!is_trace_target(pid, ctx->id))
    return 0;

  event.uid = bpf_get_current_uid_gid();
  event.cgid = bpf_get_current_cgroup_id();
  event.ts = bpf_ktime_get_ns();
  event.syscall_nr = ctx->id;
  bpf_get_current_comm(&event.comm, sizeof(event.comm));

  bpf_map_update_elem(&sys_enter_entries, &pid, &event, 0);
  return 0;
}

static __always_inline int
trace_sys_exit(struct trace_event_raw_sys_exit *ctx) {

  struct sys_enter_event_t *ap;
  u32 pid = bpf_get_current_pid_tgid() >> 32;

  if (!is_trace_target(pid, ctx->id))
    return 0;

  ap = bpf_map_lookup_elem(&sys_enter_entries, &pid);
  if (!ap)
    return 0;

  u64 end = bpf_ktime_get_ns();

  struct sys_exit_event_t event = {};
  event.pid = pid;
  event.uid = bpf_get_current_uid_gid();
  event.cgid = ap->cgid;
  event.syscall_nr = ctx->id;
  event.latency = end - ap->ts;
  bpf_get_current_comm(&event.comm, sizeof(event.comm));

  bpf_perf_event_output(ctx, &sys_exit_events, BPF_F_CURRENT_CPU, &event,
                        sizeof(event));
  return 0;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int tracepoint__raw_syscalls__sys_enter(struct trace_event_raw_sys_enter *ctx) {
  return trace_sys_enter(ctx);
}

SEC("tracepoint/raw_syscalls/sys_exit")
int tracepoint__raw_syscalls__sys_exit(struct trace_event_raw_sys_exit *ctx) {
  return trace_sys_exit(ctx);
}

char LICENSE[] SEC("license") = "GPL";
