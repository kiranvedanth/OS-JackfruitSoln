# Multi-Container Runtime with Memory Monitor

## Team Information
- Vaibhav (SRN: PES1UG24CS640)
- Vedanth (SRN: PES1UG24CS523)
- GitHub username: `kiranvedanth`

## Project Overview
This project implements a user-space multi-container runtime (`engine`) and a kernel memory monitor module (`monitor.ko`).

- `engine supervisor <base-rootfs>` starts a long-running parent supervisor.
- CLI clients (`start`, `run`, `ps`, `logs`, `stop`) communicate with supervisor via UNIX domain socket.
- Container stdout/stderr are captured through pipe-based producer threads and a bounded-buffer consumer logger.
- `monitor.ko` tracks container host PIDs and enforces soft/hard RSS limits via `ioctl`.

## Build Instructions
Run from repository root:

```bash
cd boilerplate
make
```

CI-safe compile only:

```bash
make -C boilerplate ci
```

## Environment Setup
On Ubuntu 22.04/24.04 VM (Secure Boot OFF):

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

## RootFS Setup
From repository root:

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Optional: copy workloads to rootfs before launching containers:

```bash
cp boilerplate/cpu_hog ./rootfs-alpha/
cp boilerplate/io_pulse ./rootfs-beta/
cp boilerplate/memory_hog ./rootfs-alpha/
```

## Module + Runtime Commands

### 1) Load module
```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 2) Start supervisor
```bash
cd boilerplate
sudo ./engine supervisor ../rootfs-base
```

### 3) Use CLI in another terminal
```bash
cd boilerplate
sudo ./engine start alpha ../rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ../rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

Foreground run mode:
```bash
sudo ./engine run alpha ../rootfs-alpha "/cpu_hog 8" --nice 5
echo $?
```

### 4) Kernel logs and unload
```bash
dmesg | tail -n 50
sudo rmmod monitor
```

## CLI Contract
Implemented commands:

```bash
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

## Current Notes
- Per-container logs are stored under `boilerplate/logs/<id>.log`.
- Supervisor control socket path: `/tmp/mini_runtime.sock`.
- Soft-limit events and hard-limit kill events are visible in `dmesg`.

## TODO for Final Submission
- Add all 8 required screenshots with short captions.
- Add engineering analysis sections (isolation, lifecycle, IPC/sync, memory policy, scheduling).
- Add design decisions + tradeoff discussion for each major subsystem.
- Add scheduler experiment measurements and explanation.
