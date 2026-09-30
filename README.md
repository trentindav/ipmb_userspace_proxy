# IPMB Host Userspace

**Proof of Concept — not intended for production use.**

This repository contains a rough proof of concept demonstrating that a userspace tool can
mimic the presence of an IPMI device, enabling communication from a BlueField-3
DPU with its BMC over the IPMB protocol.

## Overview

The kernel-space IPMB host driver (`linux-bluefield/drivers/char/ipmi/ipmb_host.c`)
provides an IPMI character device (`/dev/ipmi0`) backed by an I2C link to the DPU's BMC.
This project replaces that kernel driver with a FUSE CUSE-based userspace implementation
that exposes the same `/dev/ipmi0` interface, forwarding IPMI messages to the BlueField-3
DPU via an alternative transport.

## Components

| Path | Description |
|---|---|
| `ipmb_host_userspace.c` | CUSE device that creates the virtual `/dev/ipmi0` |
| `ipmi_devintf_userspace.c` | IPMI device interface — marshals IPMI messages and handles the standard IPMI ioctl protocol |

## Build

```sh
# Requires libfuse3-dev
make
```

## Usage

```sh
# Create a new /dev/ipmb-1 device 
echo ipmb-dev 0x1030 > /sys/class/i2c-adapter/i2c-1/new-device
# Remove existing BlueField-3 IPMI modules if already using a linux-bluefield kernel
rmmod ipmb_host ipmi_devintf ipmi_msghandler
# Start the userspace proxy
./ipmb_host_userspace &
# Now ipmitool can be used again on the DPU
ipmitool mc info
```

## Disclaimer

This is a prototype built to explore feasibility. It is incomplete, lacks proper error
handling, and has not undergone security review. Do not deploy or rely on it in
production environments.