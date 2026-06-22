//! DeviceAgent — typed wrapper for ORCA_SLOT_PRINTER_AGENT.
//!
//! Plugins implementing this trait expose a network-connected printer
//! to the engine. The full host-side surface lives in
//! `orca::PrinterAgent` (engine/include/orca/PrinterAgent.hpp); this
//! Rust mirror covers the most common methods: connect / disconnect /
//! send_command / start_print / cancel_print, plus the inverted
//! host-emit callbacks for status + raw printer messages.
//!
//! Permission: ORCA_PERM_DEVICE_CONTROL.

use crate::abi;
use crate::error::Result;
use core::ffi::c_void;

#[repr(i32)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum PrinterState {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Error = 3,
}

/// Inverted callbacks the host hands the plugin so the plugin can
/// emit news whenever the printer reports something. Each thunk is
/// safe to call from any thread.
#[derive(Copy, Clone)]
pub struct HostEmit {
    ctx: *mut c_void,
    emit_status:
        extern "C" fn(*mut c_void, state: i32, code: i32, message: *const core::ffi::c_char),
    emit_message: extern "C" fn(*mut c_void, payload: *const core::ffi::c_char, payload_len: usize),
    is_cancelled: extern "C" fn(*mut c_void) -> i32,
}

unsafe impl Send for HostEmit {}
unsafe impl Sync for HostEmit {}

impl HostEmit {
    pub fn emit_status(&self, state: PrinterState, code: i32, message: Option<&str>) {
        let c_msg = message.and_then(|m| std::ffi::CString::new(m).ok());
        let ptr = c_msg
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(core::ptr::null());
        (self.emit_status)(self.ctx, state as i32, code, ptr);
    }

    pub fn emit_message_bytes(&self, payload: &[u8]) {
        (self.emit_message)(self.ctx, payload.as_ptr() as *const _, payload.len());
    }

    pub fn emit_message_text(&self, payload: &str) {
        self.emit_message_bytes(payload.as_bytes());
    }

    pub fn is_cancelled(&self) -> bool {
        (self.is_cancelled)(self.ctx) != 0
    }
}

/// What plugin authors implement to expose a printer.
pub trait DeviceAgent: Send + Sync + 'static {
    /// Stable identifier the engine uses to look up this agent type
    /// (e.g. "moonraker", "bambu", "octoprint").
    const AGENT_ID: &'static str;
    const AGENT_NAME: &'static str = Self::AGENT_ID;
    const AGENT_VERSION: &'static str = "0.0.0";
    const AGENT_DESCRIPTION: &'static str = "";

    fn new(host: HostEmit) -> Self;

    fn connect(
        &self,
        device_id: &str,
        host_or_ip: &str,
        port: i32,
        username: &str,
        password: &str,
        use_tls: bool,
    ) -> Result<()>;
    fn disconnect(&self) -> Result<()>;
    fn current_state(&self) -> PrinterState;
    fn send_command(&self, payload: &[u8]) -> Result<()>;
    fn start_print(&self, gcode_path: &str, job_name: &str, start_immediately: bool) -> Result<()>;
    fn cancel_print(&self) -> Result<()>;
}

/// Register a DeviceAgent type with the engine via host vtable.
/// The engine instantiates A by calling A::new(host_emit) lazily when
/// the user picks this agent type for a configured printer.
pub fn register<A: DeviceAgent>() -> Result<()> {
    let ctx = crate::ctx::current().ok_or(crate::Error::InvalidArgument)?;
    let host = unsafe { &*ctx.host() };

    // Vtable layout matches the orca_slot_printer_agent_t prefix in
    // plugin_api.h (Phase 2.4). All function pointers are extern "C"
    // and never null; lifecycle calls go through thunks below.
    #[repr(C)]
    struct PrinterAgentVtable {
        struct_size: u32,
        agent_id: *const core::ffi::c_char,
        agent_name: *const core::ffi::c_char,
        agent_version: *const core::ffi::c_char,
        agent_description: *const core::ffi::c_char,
        create_instance: extern "C" fn(host: *const c_void, ud: *mut c_void) -> *mut c_void,
        destroy_instance: extern "C" fn(instance: *mut c_void),
        connect: extern "C" fn(
            instance: *mut c_void,
            device_id: *const core::ffi::c_char,
            host_or_ip: *const core::ffi::c_char,
            port: i32,
            username: *const core::ffi::c_char,
            password: *const core::ffi::c_char,
            use_tls: i32,
        ) -> i32,
        disconnect: extern "C" fn(instance: *mut c_void) -> i32,
        current_state: extern "C" fn(instance: *mut c_void) -> i32,
        send_command: extern "C" fn(
            instance: *mut c_void,
            payload: *const core::ffi::c_char,
            payload_len: usize,
        ) -> i32,
        start_print: extern "C" fn(
            instance: *mut c_void,
            gcode_path: *const core::ffi::c_char,
            job_name: *const core::ffi::c_char,
            start_immediately: i32,
        ) -> i32,
        cancel_print: extern "C" fn(instance: *mut c_void) -> i32,
    }

    // Thunks bridge raw C calls into the typed A: DeviceAgent. Each
    // casts `instance` back to `&A` and adapts arguments.
    extern "C" fn create<A: DeviceAgent>(host: *const c_void, _ud: *mut c_void) -> *mut c_void {
        // The host C struct is orca_printer_host_t whose first fields
        // we mirror here — only the function pointers we actually use.
        #[repr(C)]
        struct CHost {
            struct_size: u32,
            ctx: *mut c_void,
            emit_status: extern "C" fn(*mut c_void, i32, i32, *const core::ffi::c_char),
            emit_message: extern "C" fn(*mut c_void, *const core::ffi::c_char, usize),
            is_cancelled: extern "C" fn(*mut c_void) -> i32,
        }
        if host.is_null() {
            return core::ptr::null_mut();
        }
        let h = unsafe { &*(host as *const CHost) };
        let emit = HostEmit {
            ctx: h.ctx,
            emit_status: h.emit_status,
            emit_message: h.emit_message,
            is_cancelled: h.is_cancelled,
        };
        Box::into_raw(Box::new(A::new(emit))) as *mut c_void
    }
    extern "C" fn destroy<A: DeviceAgent>(instance: *mut c_void) {
        if instance.is_null() {
            return;
        }
        unsafe {
            drop(Box::from_raw(instance as *mut A));
        }
    }
    extern "C" fn current_state_thunk<A: DeviceAgent>(instance: *mut c_void) -> i32 {
        if instance.is_null() {
            return PrinterState::Disconnected as i32;
        }
        let a = unsafe { &*(instance as *const A) };
        a.current_state() as i32
    }
    extern "C" fn connect_thunk<A: DeviceAgent>(
        instance: *mut c_void,
        device_id: *const core::ffi::c_char,
        host_or_ip: *const core::ffi::c_char,
        port: i32,
        username: *const core::ffi::c_char,
        password: *const core::ffi::c_char,
        use_tls: i32,
    ) -> i32 {
        if instance.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let a = unsafe { &*(instance as *const A) };
        let to_str = |p: *const core::ffi::c_char| -> &str {
            if p.is_null() {
                ""
            } else {
                unsafe { core::ffi::CStr::from_ptr(p).to_str().unwrap_or("") }
            }
        };
        match a.connect(
            to_str(device_id),
            to_str(host_or_ip),
            port,
            to_str(username),
            to_str(password),
            use_tls != 0,
        ) {
            Ok(()) => abi::ORCA_OK,
            Err(e) => e.as_code(),
        }
    }
    extern "C" fn disconnect_thunk<A: DeviceAgent>(instance: *mut c_void) -> i32 {
        if instance.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let a = unsafe { &*(instance as *const A) };
        a.disconnect()
            .map(|_| abi::ORCA_OK)
            .unwrap_or_else(|e| e.as_code())
    }
    extern "C" fn send_command_thunk<A: DeviceAgent>(
        instance: *mut c_void,
        payload: *const core::ffi::c_char,
        payload_len: usize,
    ) -> i32 {
        if instance.is_null() || payload.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let a = unsafe { &*(instance as *const A) };
        let bytes = unsafe { core::slice::from_raw_parts(payload as *const u8, payload_len) };
        a.send_command(bytes)
            .map(|_| abi::ORCA_OK)
            .unwrap_or_else(|e| e.as_code())
    }
    extern "C" fn start_print_thunk<A: DeviceAgent>(
        instance: *mut c_void,
        gcode_path: *const core::ffi::c_char,
        job_name: *const core::ffi::c_char,
        start_immediately: i32,
    ) -> i32 {
        if instance.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let a = unsafe { &*(instance as *const A) };
        let to_str = |p: *const core::ffi::c_char| -> &str {
            if p.is_null() {
                ""
            } else {
                unsafe { core::ffi::CStr::from_ptr(p).to_str().unwrap_or("") }
            }
        };
        a.start_print(to_str(gcode_path), to_str(job_name), start_immediately != 0)
            .map(|_| abi::ORCA_OK)
            .unwrap_or_else(|e| e.as_code())
    }
    extern "C" fn cancel_print_thunk<A: DeviceAgent>(instance: *mut c_void) -> i32 {
        if instance.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let a = unsafe { &*(instance as *const A) };
        a.cancel_print()
            .map(|_| abi::ORCA_OK)
            .unwrap_or_else(|e| e.as_code())
    }

    // Identity strings need a stable pointer for the engine's lifetime.
    // Box::leak each into a heap-allocated CString and store the raw
    // pointer in the vtable; CStrings live for the rest of the process.
    let id = Box::leak(Box::new(
        std::ffi::CString::new(A::AGENT_ID).map_err(|_| crate::Error::InvalidArgument)?,
    ));
    let name = Box::leak(Box::new(
        std::ffi::CString::new(A::AGENT_NAME).map_err(|_| crate::Error::InvalidArgument)?,
    ));
    let ver = Box::leak(Box::new(
        std::ffi::CString::new(A::AGENT_VERSION).map_err(|_| crate::Error::InvalidArgument)?,
    ));
    let desc = Box::leak(Box::new(
        std::ffi::CString::new(A::AGENT_DESCRIPTION).map_err(|_| crate::Error::InvalidArgument)?,
    ));

    let vtable = Box::leak(Box::new(PrinterAgentVtable {
        struct_size: core::mem::size_of::<PrinterAgentVtable>() as u32,
        agent_id: id.as_ptr(),
        agent_name: name.as_ptr(),
        agent_version: ver.as_ptr(),
        agent_description: desc.as_ptr(),
        create_instance: create::<A>,
        destroy_instance: destroy::<A>,
        connect: connect_thunk::<A>,
        disconnect: disconnect_thunk::<A>,
        current_state: current_state_thunk::<A>,
        send_command: send_command_thunk::<A>,
        start_print: start_print_thunk::<A>,
        cancel_print: cancel_print_thunk::<A>,
    }));

    crate::error::check((host.register_printer_agent)(
        id.as_ptr(),
        vtable as *const _ as *const c_void,
        core::ptr::null_mut(),
    ))
}
