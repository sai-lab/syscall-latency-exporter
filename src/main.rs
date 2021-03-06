use std::str;

use libbpf_rs::PerfBufferBuilder;
use plain::Plain;
use structopt::StructOpt;

mod bpf;
mod syscalls;
use bpf::*;

#[repr(C)]
#[derive(Default, Debug)]
struct SysExitEvent {
    pub pid: u32,
    pub uid: u32,
    pub cgid: u64,
    pub syscall_nr: u32,
    pub latency: u64,
    pub comm: [u8; 32],
}

unsafe impl Plain for SysExitEvent {}

#[derive(Debug, StructOpt)]
struct Command {
    #[structopt(short = "c", long = "container-only")]
    container_only: bool,
    #[structopt(short = "s", long = "syscall")]
    // syscall: Vec<String>,
    syscall: Option<String>,
}

fn handle_event(_cpu: i32, data: &[u8]) {
    let mut event = SysExitEvent::default();
    plain::copy_from_bytes(&mut event, data).expect("Data buffer was too short or invalid");

    let comm = str::from_utf8(&event.comm).unwrap().trim_end_matches('\0');

    let syscall = syscalls::SYSCALLS
        .get(&(*&event.syscall_nr as u32))
        .unwrap_or(&"unknown");

    println!(
        "{:8} {:8} {:8} {:32} {: <8.8} {:16}",
        event.pid,
        event.uid,
        event.cgid,
        syscall,
        event.latency as f64 / 1000_000_000.0,
        comm
    )
}

fn handle_lost_event(cpu: i32, count: u64) {
    eprintln!("Lost event (CPU: {}, COUNT: {})", cpu, count)
}

fn main() -> anyhow::Result<()> {
    let opts: Command = Command::from_args();

    let mut skel_builder = SyslatencySkelBuilder::default();
    let mut syslatency_skel = skel_builder.open()?;
    syslatency_skel.rodata().pid_self = std::process::id();
    syslatency_skel.rodata().only_trace_container = opts.container_only as u8;

    if let Some(syscall) = opts.syscall {
        syslatency_skel.rodata().trace_syscall = syscalls::to_syscall_number(&syscall)[0] as i32
    }

    let mut skel = syslatency_skel.load()?;
    skel.attach()?;

    let perf = PerfBufferBuilder::new(skel.maps().sys_exit_events())
        .sample_cb(handle_event)
        .lost_cb(handle_lost_event)
        .build()?;

    println!(
        "{:8} {:8} {:8} {:32} {:16} {:16}",
        "PID", "UID", "CGROUP_ID", "SYSCALL", "LATENCY", "COMMAND"
    );

    loop {
        perf.poll(std::time::Duration::from_millis(100))?;
    }
}
