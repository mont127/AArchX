//! macOS Mach message ABI types omitted from bindgen, using the SDK's LP64 layout.

use core::mem::{offset_of, size_of};

pub(super) type mach_port_t = libc::mach_port_t;
pub(super) type semaphore_t = mach_port_t;
pub(super) type vm_map_t = libc::vm_map_t;
pub(super) type kern_return_t = libc::kern_return_t;

pub(super) const MACH_MSG_OOL_PORTS_DESCRIPTOR: u32 = 2;
pub(super) const MACH_MSGH_BITS_COMPLEX: u32 = 0x8000_0000;
pub(super) const MACH_PORT_NULL: mach_port_t = 0;
pub(super) const MACH_PORT_DEAD: mach_port_t = u32::MAX;

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) struct MachMsgTypeDescriptor {
    pub(super) pad1: u32,
    pub(super) pad2: u32,
    pub(super) bits: u32,
}

impl MachMsgTypeDescriptor {
    #[inline(always)]
    pub(super) fn descriptor_type(&self) -> u8 {
        (self.bits >> 24) as u8
    }

    #[inline(always)]
    pub(super) fn set_descriptor_type(&mut self, value: u8) {
        self.bits = (self.bits & 0x00ff_ffff) | ((value as u32) << 24);
    }
}

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) struct MachMsgPortDescriptor {
    pub(super) name: mach_port_t,
    pub(super) pad1: u32,
    pub(super) bits: u32,
}

impl MachMsgPortDescriptor {
    #[inline(always)]
    pub(super) fn pad2(&self) -> u16 {
        self.bits as u16
    }

    #[inline(always)]
    pub(super) fn set_pad2(&mut self, value: u16) {
        self.bits = (self.bits & 0xffff_0000) | value as u32;
    }

    #[inline(always)]
    pub(super) fn disposition(&self) -> u8 {
        (self.bits >> 16) as u8
    }

    #[inline(always)]
    pub(super) fn set_disposition(&mut self, value: u8) {
        self.bits = (self.bits & 0xff00_ffff) | ((value as u32) << 16);
    }

    #[inline(always)]
    pub(super) fn descriptor_type(&self) -> u8 {
        (self.bits >> 24) as u8
    }

    #[inline(always)]
    pub(super) fn set_descriptor_type(&mut self, value: u8) {
        self.bits = (self.bits & 0x00ff_ffff) | ((value as u32) << 24);
    }
}

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) struct MachMsgOolDescriptor {
    pub(super) address: u64,
    pub(super) bits: u32,
    pub(super) size: u32,
}

impl MachMsgOolDescriptor {
    #[inline(always)]
    pub(super) fn deallocate(&self) -> u8 {
        self.bits as u8
    }

    #[inline(always)]
    pub(super) fn set_deallocate(&mut self, value: u8) {
        self.bits = (self.bits & 0xffff_ff00) | value as u32;
    }

    #[inline(always)]
    pub(super) fn copy(&self) -> u8 {
        (self.bits >> 8) as u8
    }

    #[inline(always)]
    pub(super) fn set_copy(&mut self, value: u8) {
        self.bits = (self.bits & 0xffff_00ff) | ((value as u32) << 8);
    }

    #[inline(always)]
    pub(super) fn pad1(&self) -> u8 {
        (self.bits >> 16) as u8
    }

    #[inline(always)]
    pub(super) fn set_pad1(&mut self, value: u8) {
        self.bits = (self.bits & 0xff00_ffff) | ((value as u32) << 16);
    }

    #[inline(always)]
    pub(super) fn descriptor_type(&self) -> u8 {
        (self.bits >> 24) as u8
    }

    #[inline(always)]
    pub(super) fn set_descriptor_type(&mut self, value: u8) {
        self.bits = (self.bits & 0x00ff_ffff) | ((value as u32) << 24);
    }
}

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) struct MachMsgOolPortsDescriptor {
    pub(super) address: u64,
    pub(super) bits: u32,
    pub(super) count: u32,
}

impl MachMsgOolPortsDescriptor {
    #[inline(always)]
    pub(super) fn deallocate(&self) -> u8 {
        self.bits as u8
    }

    #[inline(always)]
    pub(super) fn set_deallocate(&mut self, value: u8) {
        self.bits = (self.bits & 0xffff_ff00) | value as u32;
    }

    #[inline(always)]
    pub(super) fn copy(&self) -> u8 {
        (self.bits >> 8) as u8
    }

    #[inline(always)]
    pub(super) fn set_copy(&mut self, value: u8) {
        self.bits = (self.bits & 0xffff_00ff) | ((value as u32) << 8);
    }

    #[inline(always)]
    pub(super) fn disposition(&self) -> u8 {
        (self.bits >> 16) as u8
    }

    #[inline(always)]
    pub(super) fn set_disposition(&mut self, value: u8) {
        self.bits = (self.bits & 0xff00_ffff) | ((value as u32) << 16);
    }

    #[inline(always)]
    pub(super) fn descriptor_type(&self) -> u8 {
        (self.bits >> 24) as u8
    }

    #[inline(always)]
    pub(super) fn set_descriptor_type(&mut self, value: u8) {
        self.bits = (self.bits & 0x00ff_ffff) | ((value as u32) << 24);
    }
}

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) struct MachMsgGuardedPortDescriptor {
    pub(super) context: u64,
    pub(super) bits: u32,
    pub(super) name: mach_port_t,
}

impl MachMsgGuardedPortDescriptor {
    #[inline(always)]
    pub(super) fn flags(&self) -> u16 {
        self.bits as u16
    }

    #[inline(always)]
    pub(super) fn set_flags(&mut self, value: u16) {
        self.bits = (self.bits & 0xffff_0000) | value as u32;
    }

    #[inline(always)]
    pub(super) fn disposition(&self) -> u8 {
        (self.bits >> 16) as u8
    }

    #[inline(always)]
    pub(super) fn set_disposition(&mut self, value: u8) {
        self.bits = (self.bits & 0xff00_ffff) | ((value as u32) << 16);
    }

    #[inline(always)]
    pub(super) fn descriptor_type(&self) -> u8 {
        (self.bits >> 24) as u8
    }

    #[inline(always)]
    pub(super) fn set_descriptor_type(&mut self, value: u8) {
        self.bits = (self.bits & 0x00ff_ffff) | ((value as u32) << 24);
    }
}

#[repr(C, packed(4))]
#[derive(Clone, Copy)]
pub(super) union MachMsgDescriptor {
    pub(super) port: MachMsgPortDescriptor,
    pub(super) out_of_line: MachMsgOolDescriptor,
    pub(super) ool_ports: MachMsgOolPortsDescriptor,
    pub(super) type_: MachMsgTypeDescriptor,
    pub(super) guarded_port: MachMsgGuardedPortDescriptor,
}

const _: () = assert!(size_of::<MachMsgTypeDescriptor>() == 12);
const _: () = assert!(offset_of!(MachMsgTypeDescriptor, pad1) == 0);
const _: () = assert!(offset_of!(MachMsgTypeDescriptor, pad2) == 4);
const _: () = assert!(offset_of!(MachMsgTypeDescriptor, bits) == 8);
const _: () = assert!(size_of::<MachMsgPortDescriptor>() == 12);
const _: () = assert!(offset_of!(MachMsgPortDescriptor, name) == 0);
const _: () = assert!(offset_of!(MachMsgPortDescriptor, pad1) == 4);
const _: () = assert!(offset_of!(MachMsgPortDescriptor, bits) == 8);
const _: () = assert!(size_of::<MachMsgOolDescriptor>() == 16);
const _: () = assert!(offset_of!(MachMsgOolDescriptor, address) == 0);
const _: () = assert!(offset_of!(MachMsgOolDescriptor, bits) == 8);
const _: () = assert!(offset_of!(MachMsgOolDescriptor, size) == 12);
const _: () = assert!(size_of::<MachMsgOolPortsDescriptor>() == 16);
const _: () = assert!(offset_of!(MachMsgOolPortsDescriptor, address) == 0);
const _: () = assert!(offset_of!(MachMsgOolPortsDescriptor, bits) == 8);
const _: () = assert!(offset_of!(MachMsgOolPortsDescriptor, count) == 12);
const _: () = assert!(size_of::<MachMsgGuardedPortDescriptor>() == 16);
const _: () = assert!(offset_of!(MachMsgGuardedPortDescriptor, context) == 0);
const _: () = assert!(offset_of!(MachMsgGuardedPortDescriptor, bits) == 8);
const _: () = assert!(offset_of!(MachMsgGuardedPortDescriptor, name) == 12);
const _: () = assert!(size_of::<MachMsgDescriptor>() == 16);
const _: () = assert!(offset_of!(MachMsgDescriptor, port) == 0);
const _: () = assert!(offset_of!(MachMsgDescriptor, out_of_line) == 0);
const _: () = assert!(offset_of!(MachMsgDescriptor, ool_ports) == 0);
const _: () = assert!(offset_of!(MachMsgDescriptor, type_) == 0);
const _: () = assert!(offset_of!(MachMsgDescriptor, guarded_port) == 0);
