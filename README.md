# Multi-Container Runtime with Memory Monitor

## Team Information
- Vaibhav (SRN: PES1UG24CS640)
- Vedanth (SRN: PES1UG24CS523)
- GitHub username: `kiranvedanth`

## 🔹 Overview

This project implements a lightweight container runtime in C, inspired by core Linux container principles. It demonstrates how operating system concepts like process isolation, scheduling, and kernel interaction work together in a practical system.

### 🔧 Key Features

* Multi-container execution using Linux namespaces
* Centralized supervisor process
* Per-container logging system
* Kernel-level monitoring using a Loadable Kernel Module (LKM)
* CPU vs I/O scheduling experiments
* Clean lifecycle management (no zombie processes)

---

## 🔹 Build, Load, and Run Instructions

### 1. Build the Project

```bash
cd boilerplate
make
```

---

### 2. Load Kernel Module

```bash
sudo insmod monitor.ko
sudo dmesg | tail -n 20
```

---

### 3. Start Supervisor

```bash
sudo ./engine supervisor ../rootfs-base
```

---

### 4. Prepare Container Root Filesystems

```bash
sudo cp -a ../rootfs-base ../rootfs-alpha
sudo cp -a ../rootfs-base ../rootfs-beta
```

---

### 5. Start Containers

```bash
sudo ./engine start alpha ../rootfs-alpha /cpu_hog
sudo ./engine start beta ../rootfs-beta /io_pulse
```

---

### 6. Inspect Containers

```bash
sudo ./engine ps
```

---

### 7. View Logs

```bash
sudo ./engine logs alpha
```

---

### 8. Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

## 📸 Demo with Screenshots

**1. Multi-container supervision**

![SCREENSHOT 1](https://github.com/user-attachments/assets/7f53dca6-9c14-4234-a84b-008df77b5b17)
> The supervisor engine concurrently managing multiple active containers ('alpha' and 'beta') under a single parent process.

---

**2. Metadata tracking (ps)**

![SCREENSHOT 2](https://github.com/user-attachments/assets/bf504535-9140-4827-a942-357da81aa328)
> The CLI 'ps' command querying the supervisor to display dynamically tracked container metadata, including assigned process IDs (PIDs) and states.

---

**3. Bounded-buffer logging**

![SCREENSHOT 3](https://github.com/user-attachments/assets/eeef4e16-0908-4404-bcd4-4a8d6664f02c)
> Successful retrieval of container execution logs, verifying that the pipeline's consumer thread is actively capturing and writing standard output to disk.

---

**4. CLI + IPC**

![SCREENSHOT 4](https://github.com/user-attachments/assets/0741d738-6702-4a32-9ca6-36c1323610a8)
> Issuing a control command via the CLI client, which successfully communicates with the background supervisor through the Unix Domain Socket to terminate a container.

---

**5. Soft-limit warning**

![SCREENSHOT 5](https://github.com/user-attachments/assets/94c2758c-45e1-463e-b68f-785236dfc7bc)
> CLI execution starting container 'gamma' with strict memory boundaries to trigger kernel-level monitoring.

---

**6. Hard-limit enforcement**

![SCREENSHOT 6](https://github.com/user-attachments/assets/c4fadb67-f9d9-4ad6-a7d3-39488987943f)
> The custom Loadable Kernel Module detecting a memory boundary breach by container 'gamma' and successfully enforcing the constraint.

---

**7. Scheduling experiment**

![SCREENSHOT 7](https://github.com/user-attachments/assets/5650e696-3eb9-43e3-bda7-4b730ee7fb6d)
> Process monitor confirming that the isolated 'cpu_hog' container is effectively scheduled by the OS and saturating its allocated CPU time slice.

---

**8. Clean teardown**

![SCREENSHOT 8](https://github.com/user-attachments/assets/8cf926a0-3a0d-4cd2-8ed0-06c5a5d1cd97)
> The supervisor gracefully intercepting a SIGINT (^C) signal, triggering the cleanup routine to close the IPC socket and reap all child processes to prevent zombies.

---

## 🔹 Engineering Analysis

**How The System Works**

We put programs in their own private space. This makes sure they cannot see or touch other programs. We change their root folder so they are locked in.

For memory limits we use a kernel module. The kernel is the deep core of the system. It can see exactly how much memory a program uses and kill it instantly if it takes too much. Normal user programs cannot do this safely.

## 🔹 Design Choices

**Isolation**
Choice - We used Linux namespaces
Good - It is very fast and lightweight
Bad - It is not as secure as a full virtual machine

**Supervisor**
Choice - We made one main background program to manage everything
Good - It makes tracking all the containers very simple
Bad - If the main program breaks everything breaks

**Logging**
Choice - We used memory buffers and sockets
Good - The main program does not freeze while waiting for logs to save to the disk
Bad - It uses a little bit more memory to run

**Kernel Monitor**
Choice - We wrote custom core kernel code
Good - It is the only way to perfectly stop memory leaks at the lowest level
Bad - A mistake in the code can crash the whole computer

**Scheduling Tests**
Choice - We used simple math and sleep programs
Good - It is very easy to see exactly how the computer shares its time
Bad - It does not test real world messy programs

## 🔹 Scheduler Test Results

Here is what happened when we ran our two test programs at the same time

**The CPU Hog Program**
This program does heavy math and never stops
The system gave it over 90 percent of the processing power

**The IO Pulse Program**
This program does very small tasks and then goes to sleep
The system gave it almost 0 percent of the processing power

**What This Proves**
The Linux system is smart. It gives power to the programs doing hard work and takes power away from programs that are resting. This keeps the whole computer running smoothly without freezing.

## 🔹 Conclusion

This project builds a real working container system from scratch
It brings together process separation and custom logging
It also links user space programs with a real Linux kernel module
Building this showed exactly how an operating system controls memory and processes behind the scenes
