use std::ffi::{c_char, c_int, c_void};
use std::ptr::{self, NonNull};

pub const OK: i32 = 0;
pub const ERROR_WOULD_BLOCK: i32 = 5;
pub const ERROR_CLOSED: i32 = 6;

#[repr(C)]
pub struct TinyxServer {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TinyxClient {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Default)]
struct HostOps {
    struct_size: u32,
    monotonic_time_ms: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    log: Option<unsafe extern "C" fn(*mut c_void, c_int, *const c_char)>,
    wakeup: Option<unsafe extern "C" fn(*mut c_void)>,
    leds_changed: Option<unsafe extern "C" fn(*mut c_void, u32)>,
    bell: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int)>,
}

#[repr(C)]
struct FramebufferConfig {
    struct_size: u32,
    width: u32,
    height: u32,
    width_mm: u32,
    height_mm: u32,
    stride_bytes: usize,
    pixels: *mut c_void,
    pixels_size: usize,
}

#[repr(C)]
struct ScreenConfig {
    struct_size: u32,
    framebuffer: FramebufferConfig,
    depths: *const c_void,
    depth_count: usize,
    root_depth_index: usize,
    root_visual_index: usize,
}

#[repr(C)]
struct Config {
    struct_size: u32,
    api_version_major: u32,
    api_version_minor: u32,
    host: HostOps,
    host_userdata: *mut c_void,
    initial_screen: *const ScreenConfig,
}

#[repr(C)]
struct ClientConfig {
    struct_size: u32,
    input_buffer_limit: usize,
    output_buffer_limit: usize,
}

#[repr(C)]
struct Error {
    status: c_int,
    message: [c_char; 1024],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct StepResult {
    pub requests_processed: u32,
    pub immediate_work: c_int,
    pub generation_finished: c_int,
    pub next_timeout_ms: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct FramebufferInfo {
    pixels: *const c_void,
    size: usize,
    width: u32,
    height: u32,
    stride_bytes: usize,
    depth: u32,
    bits_per_pixel: u32,
    red_mask: u32,
    green_mask: u32,
    blue_mask: u32,
    byte_order: c_int,
}

impl Default for FramebufferInfo {
    fn default() -> Self {
        Self {
            pixels: ptr::null(),
            size: 0,
            width: 0,
            height: 0,
            stride_bytes: 0,
            depth: 0,
            bits_per_pixel: 0,
            red_mask: 0,
            green_mask: 0,
            blue_mask: 0,
            byte_order: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DamageRect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

unsafe extern "C" {
    fn tinyx_framebuffer_config_init(config: *mut FramebufferConfig);
    fn tinyx_screen_config_init(config: *mut ScreenConfig);
    fn tinyx_config_init(config: *mut Config);
    fn tinyx_client_config_init(config: *mut ClientConfig);
    fn tinyx_server_create(
        config: *const Config,
        out_server: *mut *mut TinyxServer,
        error: *mut Error,
    ) -> c_int;
    fn tinyx_server_destroy(server: *mut TinyxServer) -> c_int;
    fn tinyx_server_step(
        server: *mut TinyxServer,
        request_budget: u32,
        result: *mut StepResult,
    ) -> c_int;
    fn tinyx_server_resize(
        server: *mut TinyxServer,
        framebuffer: *const FramebufferConfig,
    ) -> c_int;
    fn tinyx_client_open(
        server: *mut TinyxServer,
        config: *const ClientConfig,
        out_client: *mut *mut TinyxClient,
    ) -> c_int;
    fn tinyx_client_send(
        client: *mut TinyxClient,
        bytes: *const c_void,
        length: usize,
        out_length: *mut usize,
    ) -> c_int;
    fn tinyx_client_receive(
        client: *mut TinyxClient,
        bytes: *mut c_void,
        capacity: usize,
        out_length: *mut usize,
    ) -> c_int;
    fn tinyx_client_shutdown_send(client: *mut TinyxClient) -> c_int;
    fn tinyx_client_is_closed(client: *const TinyxClient) -> c_int;
    fn tinyx_client_destroy(client: *mut TinyxClient);
    fn tinyx_server_get_framebuffer(server: *mut TinyxServer, info: *mut FramebufferInfo) -> c_int;
    fn tinyx_server_take_damage(
        server: *mut TinyxServer,
        rects: *mut DamageRect,
        capacity: usize,
        out_count: *mut usize,
    ) -> c_int;
    fn tinyx_pointer_motion_absolute(server: *mut TinyxServer, x: i32, y: i32) -> c_int;
    fn tinyx_pointer_button(server: *mut TinyxServer, button: u32, pressed: c_int) -> c_int;
    fn tinyx_key(server: *mut TinyxServer, keycode: u32, pressed: c_int) -> c_int;
    fn tinyx_release_all_keys(server: *mut TinyxServer) -> c_int;
}

fn status(operation: &str, value: i32) -> Result<(), String> {
    if value == OK {
        Ok(())
    } else {
        Err(format!("{operation} failed with TinyX status {value}"))
    }
}

pub struct Server {
    raw: NonNull<TinyxServer>,
}

impl Server {
    pub fn create(width: u32, height: u32, width_mm: u32, height_mm: u32) -> Result<Self, String> {
        let mut screen = ScreenConfig {
            struct_size: 0,
            framebuffer: FramebufferConfig {
                struct_size: 0,
                width: 0,
                height: 0,
                width_mm: 0,
                height_mm: 0,
                stride_bytes: 0,
                pixels: ptr::null_mut(),
                pixels_size: 0,
            },
            depths: ptr::null(),
            depth_count: 0,
            root_depth_index: 0,
            root_visual_index: 0,
        };
        let mut config = Config {
            struct_size: 0,
            api_version_major: 0,
            api_version_minor: 0,
            host: HostOps::default(),
            host_userdata: ptr::null_mut(),
            initial_screen: ptr::null(),
        };
        let mut error = Error {
            status: OK,
            message: [0; 1024],
        };
        let mut raw = ptr::null_mut();
        unsafe {
            tinyx_screen_config_init(&mut screen);
            screen.framebuffer.width = width;
            screen.framebuffer.height = height;
            screen.framebuffer.width_mm = width_mm;
            screen.framebuffer.height_mm = height_mm;
            tinyx_config_init(&mut config);
            config.initial_screen = &screen;
            let result = tinyx_server_create(&config, &mut raw, &mut error);
            if result != OK {
                let bytes = error
                    .message
                    .iter()
                    .take_while(|byte| **byte != 0)
                    .map(|byte| *byte as u8)
                    .collect::<Vec<_>>();
                let message = String::from_utf8_lossy(&bytes);
                return Err(format!("TinyX startup failed ({result}): {message}"));
            }
        }
        let raw = NonNull::new(raw).ok_or_else(|| "TinyX returned a null server".to_owned())?;
        Ok(Self { raw })
    }

    pub fn resize(&self, width: u32, height: u32) -> Result<(), String> {
        let mut framebuffer = FramebufferConfig {
            struct_size: 0,
            width: 0,
            height: 0,
            width_mm: 0,
            height_mm: 0,
            stride_bytes: 0,
            pixels: ptr::null_mut(),
            pixels_size: 0,
        };
        unsafe {
            tinyx_framebuffer_config_init(&mut framebuffer);
            framebuffer.width = width;
            framebuffer.height = height;
            status(
                "tinyx_server_resize",
                tinyx_server_resize(self.raw.as_ptr(), &framebuffer),
            )
        }
    }

    pub fn open_client(&self) -> Result<Client, String> {
        let mut config = ClientConfig {
            struct_size: 0,
            input_buffer_limit: 0,
            output_buffer_limit: 0,
        };
        let mut raw = ptr::null_mut();
        unsafe {
            tinyx_client_config_init(&mut config);
            status(
                "tinyx_client_open",
                tinyx_client_open(self.raw.as_ptr(), &config, &mut raw),
            )?;
        }
        let raw = NonNull::new(raw).ok_or_else(|| "TinyX returned a null client".to_owned())?;
        Ok(Client {
            raw,
            send_open: true,
        })
    }

    pub fn step(&self, budget: u32) -> Result<StepResult, String> {
        let mut result = StepResult::default();
        unsafe {
            status(
                "tinyx_server_step",
                tinyx_server_step(self.raw.as_ptr(), budget, &mut result),
            )?;
        }
        Ok(result)
    }

    pub fn pointer_motion(&self, x: i32, y: i32) -> Result<(), String> {
        unsafe {
            status(
                "tinyx_pointer_motion_absolute",
                tinyx_pointer_motion_absolute(self.raw.as_ptr(), x, y),
            )
        }
    }

    pub fn pointer_button(&self, button: u32, pressed: bool) -> Result<(), String> {
        unsafe {
            status(
                "tinyx_pointer_button",
                tinyx_pointer_button(self.raw.as_ptr(), button, i32::from(pressed)),
            )
        }
    }

    pub fn key(&self, keycode: u32, pressed: bool) -> Result<(), String> {
        unsafe {
            status(
                "tinyx_key",
                tinyx_key(self.raw.as_ptr(), keycode, i32::from(pressed)),
            )
        }
    }

    pub fn release_all_keys(&self) -> Result<(), String> {
        unsafe {
            status(
                "tinyx_release_all_keys",
                tinyx_release_all_keys(self.raw.as_ptr()),
            )
        }
    }

    pub fn take_damage(&self) -> Result<Vec<DamageRect>, String> {
        let mut count = 0;
        unsafe {
            status(
                "tinyx_server_take_damage",
                tinyx_server_take_damage(self.raw.as_ptr(), ptr::null_mut(), 0, &mut count),
            )?;
        }
        if count == 0 {
            return Ok(Vec::new());
        }

        let mut rects = vec![DamageRect::default(); count];
        unsafe {
            status(
                "tinyx_server_take_damage",
                tinyx_server_take_damage(
                    self.raw.as_ptr(),
                    rects.as_mut_ptr(),
                    rects.len(),
                    &mut count,
                ),
            )?;
        }
        rects.truncate(count);
        Ok(rects)
    }

    fn framebuffer_info(&self) -> Result<FramebufferInfo, String> {
        let mut info = FramebufferInfo::default();
        unsafe {
            status(
                "tinyx_server_get_framebuffer",
                tinyx_server_get_framebuffer(self.raw.as_ptr(), &mut info),
            )?;
        }
        if info.bits_per_pixel != 32
            || info.depth != 24
            || info.red_mask != 0x00ff_0000
            || info.green_mask != 0x0000_ff00
            || info.blue_mask != 0x0000_00ff
            || info.width == 0
            || info.height == 0
            || info.stride_bytes < info.width as usize * 4
            || info.pixels.is_null()
            || info
                .stride_bytes
                .checked_mul(info.height as usize)
                .is_none_or(|required| required > info.size)
        {
            return Err("TinyX returned an unsupported framebuffer layout".to_owned());
        }
        Ok(info)
    }

    pub fn rgb(&self, rect: DamageRect) -> Result<Vec<u8>, String> {
        let info = self.framebuffer_info()?;
        if rect.x < 0
            || rect.y < 0
            || rect.width == 0
            || rect.height == 0
            || rect.x as u32 > info.width.saturating_sub(rect.width)
            || rect.y as u32 > info.height.saturating_sub(rect.height)
        {
            return Err("damage rectangle lies outside the framebuffer".to_owned());
        }

        let pixels = unsafe { std::slice::from_raw_parts(info.pixels.cast::<u8>(), info.size) };
        let mut rgb = Vec::with_capacity(rect.width as usize * rect.height as usize * 3);
        for y in rect.y as usize..(rect.y as usize + rect.height as usize) {
            let x = rect.x as usize * 4;
            let row = &pixels[y * info.stride_bytes + x..][..rect.width as usize * 4];
            for bytes in row.chunks_exact(4) {
                let pixel = match info.byte_order {
                    0 => u32::from_le_bytes(bytes.try_into().unwrap()),
                    1 => u32::from_be_bytes(bytes.try_into().unwrap()),
                    _ => return Err("TinyX returned an invalid framebuffer byte order".to_owned()),
                };
                rgb.push(((pixel & info.red_mask) >> 16) as u8);
                rgb.push(((pixel & info.green_mask) >> 8) as u8);
                rgb.push((pixel & info.blue_mask) as u8);
            }
        }
        Ok(rgb)
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = unsafe { tinyx_server_destroy(self.raw.as_ptr()) };
    }
}

pub struct Client {
    raw: NonNull<TinyxClient>,
    send_open: bool,
}

impl Client {
    pub fn send(&mut self, bytes: &[u8]) -> Result<usize, i32> {
        let mut accepted = 0;
        let result = unsafe {
            tinyx_client_send(
                self.raw.as_ptr(),
                bytes.as_ptr().cast(),
                bytes.len(),
                &mut accepted,
            )
        };
        if result == OK {
            Ok(accepted)
        } else {
            Err(result)
        }
    }

    pub fn receive(&mut self, bytes: &mut [u8]) -> Result<usize, i32> {
        let mut received = 0;
        let result = unsafe {
            tinyx_client_receive(
                self.raw.as_ptr(),
                bytes.as_mut_ptr().cast(),
                bytes.len(),
                &mut received,
            )
        };
        if result == OK {
            Ok(received)
        } else {
            Err(result)
        }
    }

    pub fn is_closed(&self) -> bool {
        unsafe { tinyx_client_is_closed(self.raw.as_ptr()) != 0 }
    }

    pub fn shutdown_send(&mut self) {
        if self.send_open {
            let _ = unsafe { tinyx_client_shutdown_send(self.raw.as_ptr()) };
            self.send_open = false;
        }
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.shutdown_send();
        unsafe { tinyx_client_destroy(self.raw.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rust_ffi_starts_server_and_completes_x11_handshake() {
        let setup = [b'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        let server = Server::create(64, 64, 22, 22).unwrap();
        server.resize(80, 48).unwrap();
        let damage = server.take_damage().unwrap();
        assert_eq!(
            damage,
            vec![DamageRect {
                x: 0,
                y: 0,
                width: 80,
                height: 48
            }]
        );
        let rgb = server.rgb(damage[0]).unwrap();
        assert_eq!(rgb.len(), 80 * 48 * 3);
        let mut client = server.open_client().unwrap();
        assert_eq!(client.send(&setup), Ok(setup.len()));

        let mut reply = [0_u8; 8192];
        let mut received = 0;
        for _ in 0..100 {
            server.step(16).unwrap();
            match client.receive(&mut reply) {
                Ok(count) if count != 0 => {
                    received = count;
                    break;
                }
                Ok(_) | Err(ERROR_WOULD_BLOCK) => {}
                Err(status) => panic!("handshake receive failed with status {status}"),
            }
        }
        assert!(received > 0);
        assert_eq!(reply[0], 1);
    }
}
