#![cfg(unix)]

mod benchmark;
mod ffi;
mod input;

use std::cell::Cell;
use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::CString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::mpsc::{self, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

use base64::Engine as _;
use crossterm::cursor::{Hide, Show};
use crossterm::event::{
    self, DisableFocusChange, DisableMouseCapture, EnableFocusChange, EnableMouseCapture, Event,
    KeyCode, KeyEvent, KeyEventKind, KeyModifiers, KeyboardEnhancementFlags, MouseButton,
    MouseEventKind, PopKeyboardEnhancementFlags, PushKeyboardEnhancementFlags,
};
use crossterm::execute;
use crossterm::terminal::{
    self, EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
};
use ffi::{DamageRect, ERROR_CLOSED, ERROR_WOULD_BLOCK, Server};
use flate2::Compression;
use flate2::write::ZlibEncoder;
use input::KeyboardState;
use serde::Deserialize;

const FRAME_INTERVAL: Duration = Duration::from_millis(33);
const FALLBACK_CELL_WIDTH: u32 = 8;
const FALLBACK_CELL_HEIGHT: u32 = 16;
const HELP: &str = concat!(
    "TermX ",
    env!("CARGO_PKG_VERSION"),
    "\n",
    "TinyX terminal embedder using the Kitty graphics protocol\n\n",
    "Usage: termx [OPTIONS] [-- COMMAND [ARGUMENT...]]\n\n",
    "Options:\n",
    "  --config PATH     Configuration file path\n",
    "  --display NUMBER  X11 display number [default: 99]\n",
    "  --socket PATH     Unix-domain X11 socket path\n",
    "  --log PATH        Log file path [default: /tmp/termx-<pid>.log]\n",
    "  --trace-directory PATH  Capture per-client X11 byte streams\n",
    "  --dpi NUMBER      Override terminal DPI (1 through 1000)\n",
    "  --benchmark-kitty [PATH]  Benchmark Kitty transfers to a CSV file\n",
    "  -h, --help        Print help\n\n",
    "Command:\n",
    "  An optional client or session to launch after startup. TermX sets its\n",
    "  DISPLAY to the selected display and exits when the command exits.\n\n",
    "Configuration:\n",
    "  The default file is $XDG_CONFIG_HOME/termx/config.toml or\n",
    "  $HOME/.config/termx/config.toml. CLI values override environment values,\n",
    "  which override the configuration file.\n\n",
    "Environment:\n",
    "  TERMX_CONFIG      Default value for --config\n",
    "  TINYX_DISPLAY     Default value for --display\n",
    "  TINYX_X11_SOCKET  Default value for --socket\n",
    "  TERMX_LOG         Default value for --log\n",
    "  TERMX_TRACE_DIRECTORY  Default value for --trace-directory\n",
    "  TINYX_DPI         Default value for --dpi\n",
);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Viewport {
    columns: u16,
    image_rows: u16,
}

impl Viewport {
    fn current() -> Self {
        let (columns, rows) = terminal::size().unwrap_or((80, 24));
        Self::new(columns, rows)
    }

    fn new(columns: u16, rows: u16) -> Self {
        Self {
            columns: columns.max(1),
            image_rows: rows.max(1),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ScreenGeometry {
    width: u32,
    height: u32,
}

impl ScreenGeometry {
    fn for_viewport(viewport: Viewport) -> Self {
        let total_rows = u32::from(viewport.image_rows);
        if let Ok(size) = terminal::window_size() {
            if size.width != 0 && size.height != 0 {
                return Self {
                    width: u32::from(size.width),
                    height: (u32::from(size.height) * u32::from(viewport.image_rows) / total_rows)
                        .max(1),
                };
            }
        }
        Self {
            width: u32::from(viewport.columns) * FALLBACK_CELL_WIDTH,
            height: u32::from(viewport.image_rows) * FALLBACK_CELL_HEIGHT,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct ScreenDpi {
    x: f64,
    y: f64,
}

impl ScreenDpi {
    const DEFAULT: Self = Self { x: 75.0, y: 75.0 };

    fn uniform(value: f64) -> Self {
        Self { x: value, y: value }
    }

    fn physical_size(self, screen: ScreenGeometry) -> (u32, u32) {
        (
            millimeters(screen.width, self.x),
            millimeters(screen.height, self.y),
        )
    }
}

fn millimeters(pixels: u32, dpi: f64) -> u32 {
    (f64::from(pixels) * 25.4 / dpi).round().clamp(1.0, 32767.0) as u32
}

fn valid_dpi(value: f64) -> bool {
    value.is_finite() && (1.0..=1000.0).contains(&value)
}

fn parse_terminal_dpi(output: &str) -> Option<ScreenDpi> {
    let mut x = None;
    let mut y = None;
    for line in output.lines() {
        let (name, value) = line.split_once(':')?;
        let value = value.trim().parse::<f64>().ok()?;
        match name.trim() {
            "dpi_x" if valid_dpi(value) => x = Some(value),
            "dpi_y" if valid_dpi(value) => y = Some(value),
            _ => {}
        }
    }
    Some(ScreenDpi { x: x?, y: y? })
}

fn detect_terminal_dpi() -> Option<ScreenDpi> {
    std::env::var_os("KITTY_WINDOW_ID")?;
    let output = std::process::Command::new("kitten")
        .args(["query_terminal", "--wait-for", "1", "dpi_x", "dpi_y"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    parse_terminal_dpi(std::str::from_utf8(&output.stdout).ok()?)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ImagePlacement {
    column: u16,
    row: u16,
    columns: u16,
    rows: u16,
    pixel_size: Option<(u32, u32)>,
}

impl ImagePlacement {
    fn current(viewport: Viewport, screen: ScreenGeometry) -> Self {
        let pixel_size = terminal::window_size().ok().and_then(|size| {
            (size.width != 0 && size.height != 0)
                .then_some((u32::from(size.width), u32::from(size.height)))
        });
        Self::new(viewport, pixel_size, screen)
    }

    fn new(viewport: Viewport, pixel_size: Option<(u32, u32)>, screen: ScreenGeometry) -> Self {
        // If pixel dimensions are unavailable, assume the conventional 1:2
        // terminal-cell aspect ratio. Kitty normally supplies exact pixels.
        let (window_width, window_height) = pixel_size.unwrap_or((
            u32::from(viewport.columns),
            u32::from(viewport.image_rows) * 2,
        ));
        let total_rows = u32::from(viewport.image_rows);
        let cell_width = window_width as f64 / f64::from(viewport.columns);
        let cell_height = window_height as f64 / f64::from(total_rows);
        let available_width = cell_width * f64::from(viewport.columns);
        let available_height = cell_height * f64::from(viewport.image_rows);
        let source_aspect = f64::from(screen.width) / f64::from(screen.height);

        let (columns, rows) = if available_width / available_height > source_aspect {
            let rows = viewport.image_rows;
            let columns = (source_aspect * f64::from(rows) * cell_height / cell_width)
                .round()
                .clamp(1.0, f64::from(viewport.columns)) as u16;
            (columns, rows)
        } else {
            let columns = viewport.columns;
            let rows = (f64::from(columns) * cell_width / source_aspect / cell_height)
                .round()
                .clamp(1.0, f64::from(viewport.image_rows)) as u16;
            (columns, rows)
        };
        Self {
            column: (viewport.columns - columns) / 2,
            row: (viewport.image_rows - rows) / 2,
            columns,
            rows,
            pixel_size,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct PresentStats {
    regions: usize,
    pixels: u64,
    wire_bytes: usize,
    full_frame: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ZlibPolicy {
    Never,
    Adaptive,
    Always,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum GraphicsTransport {
    Auto,
    Direct,
    TemporaryFile,
    SharedMemory,
}

#[derive(Debug)]
struct KittyPresenter {
    image_id: u32,
    placement_id: u32,
    screen: Option<ScreenGeometry>,
    placement: Option<ImagePlacement>,
    local_media: bool,
    next_transfer: u64,
    last_medium: &'static str,
}

impl KittyPresenter {
    fn new() -> Self {
        let local_media = std::env::var_os("KITTY_WINDOW_ID").is_some()
            && std::env::var_os("SSH_CONNECTION").is_none()
            && std::env::var_os("TMUX").is_none()
            && std::env::var_os("STY").is_none();
        Self {
            image_id: 1,
            placement_id: 1,
            screen: None,
            placement: None,
            local_media,
            next_transfer: 1,
            last_medium: "direct",
        }
    }

    fn present(
        &mut self,
        writer: &mut impl Write,
        server: &Server,
        screen: ScreenGeometry,
        placement: ImagePlacement,
        damage: Vec<DamageRect>,
        force_full: bool,
    ) -> Result<PresentStats, Box<dyn std::error::Error>> {
        let initialize = self.screen != Some(screen);
        let rects = plan_damage(screen, damage, force_full || initialize);
        if rects.is_empty() && self.placement == Some(placement) {
            return Ok(PresentStats::default());
        }

        writer.write_all(b"\x1b[s")?;
        let mut stats = PresentStats::default();
        if initialize {
            let full = DamageRect {
                x: 0,
                y: 0,
                width: screen.width,
                height: screen.height,
            };
            let rgb = server.rgb(full).map_err(io::Error::other)?;
            write!(
                writer,
                "\x1b[{};{}H",
                placement.row + 1,
                placement.column + 1
            )?;
            stats.wire_bytes = self.transmit(
                writer,
                &format!(
                    "a=T,f=24,s={},v={},i={},p={},q=2,C=1,c={},r={}",
                    screen.width,
                    screen.height,
                    self.image_id,
                    self.placement_id,
                    placement.columns,
                    placement.rows
                ),
                &rgb,
                false,
            )?;
            stats.regions = 1;
            stats.pixels = u64::from(screen.width) * u64::from(screen.height);
            stats.full_frame = true;
        } else {
            if self.placement != Some(placement) {
                write!(
                    writer,
                    "\x1b[{};{}H\x1b_Ga=p,i={},p={},q=2,C=1,c={},r={};\x1b\\",
                    placement.row + 1,
                    placement.column + 1,
                    self.image_id,
                    self.placement_id,
                    placement.columns,
                    placement.rows
                )?;
            }
            for rect in &rects {
                let rgb = server.rgb(*rect).map_err(io::Error::other)?;
                stats.wire_bytes += self.transmit(
                    writer,
                    &format!(
                        "a=f,r=1,i={},f=24,q=2,x={},y={},s={},v={},X=1",
                        self.image_id, rect.x, rect.y, rect.width, rect.height
                    ),
                    &rgb,
                    true,
                )?;
                stats.pixels += u64::from(rect.width) * u64::from(rect.height);
            }
            if !rects.is_empty() {
                select_kitty_frame(writer, self.image_id, 1)?;
                stats.regions = rects.len();
                stats.full_frame = rects.len() == 1
                    && rects[0].x == 0
                    && rects[0].y == 0
                    && rects[0].width == screen.width
                    && rects[0].height == screen.height;
            }
        }
        writer.write_all(b"\x1b[u")?;
        writer.flush()?;

        self.screen = Some(screen);
        self.placement = Some(placement);
        Ok(stats)
    }

    fn transmit(
        &mut self,
        writer: &mut impl Write,
        control: &str,
        pixels: &[u8],
        animation: bool,
    ) -> io::Result<usize> {
        self.transmit_with(
            writer,
            control,
            pixels,
            animation,
            ZlibPolicy::Adaptive,
            GraphicsTransport::Auto,
            4096,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn transmit_with(
        &mut self,
        writer: &mut impl Write,
        control: &str,
        pixels: &[u8],
        animation: bool,
        zlib: ZlibPolicy,
        transport: GraphicsTransport,
        chunk_size: usize,
    ) -> io::Result<usize> {
        if chunk_size == 0 || chunk_size > 4096 || chunk_size % 4 != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Kitty direct-transfer chunks must be a nonzero multiple of four at most 4096 bytes",
            ));
        }

        let local_auto = transport == GraphicsTransport::Auto && self.local_media;
        let local_transport = matches!(
            transport,
            GraphicsTransport::SharedMemory | GraphicsTransport::TemporaryFile
        ) || local_auto;
        let compressed =
            if zlib == ZlibPolicy::Always || (zlib == ZlibPolicy::Adaptive && !local_transport) {
                let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
                encoder.write_all(pixels)?;
                Some(encoder.finish()?)
            } else {
                None
            };
        let use_compressed = match (&compressed, zlib) {
            (Some(_), ZlibPolicy::Always) => true,
            (Some(bytes), ZlibPolicy::Adaptive) => bytes.len() < pixels.len(),
            _ => false,
        };
        let (payload, compression) = if use_compressed {
            (compressed.as_deref().unwrap(), ",o=z")
        } else {
            (pixels, "")
        };

        if transport == GraphicsTransport::SharedMemory || local_auto {
            match self.write_shared_payload(payload) {
                Ok(name) => {
                    let encoded_name =
                        base64::engine::general_purpose::STANDARD.encode(name.as_bytes());
                    let result = write!(
                        writer,
                        "\x1b_G{control}{compression},t=s,S={};{encoded_name}\x1b\\",
                        payload.len()
                    );
                    if result.is_err() {
                        unsafe { libc::shm_unlink(name.as_ptr()) };
                    }
                    result?;
                    self.last_medium = "shared-memory";
                    return Ok(payload.len());
                }
                Err(error) if transport == GraphicsTransport::SharedMemory => return Err(error),
                Err(_) => {}
            }
        }

        if transport == GraphicsTransport::TemporaryFile || local_auto {
            let path = self.write_temporary_payload(payload)?;
            let encoded_path = base64::engine::general_purpose::STANDARD
                .encode(path.as_os_str().as_encoded_bytes());
            let result = write!(
                writer,
                "\x1b_G{control}{compression},t=t,S={};{encoded_path}\x1b\\",
                payload.len()
            );
            if result.is_err() {
                let _ = fs::remove_file(path);
            }
            result?;
            self.last_medium = "temporary-file";
            return Ok(payload.len());
        }

        let encoded = base64::engine::general_purpose::STANDARD.encode(payload);
        for (index, chunk) in encoded.as_bytes().chunks(chunk_size).enumerate() {
            let more = usize::from((index + 1) * chunk_size < encoded.len());
            if index == 0 {
                write!(writer, "\x1b_G{control}{compression},t=d,m={more};")?;
            } else if animation {
                write!(writer, "\x1b_Ga=f,m={more};")?;
            } else {
                write!(writer, "\x1b_Gm={more};")?;
            }
            writer.write_all(chunk)?;
            writer.write_all(b"\x1b\\")?;
        }
        self.last_medium = "direct";
        Ok(payload.len())
    }

    fn write_shared_payload(&mut self, payload: &[u8]) -> io::Result<CString> {
        loop {
            let name = CString::new(format!(
                "/tx-gfx-{:x}-{:x}",
                std::process::id(),
                self.next_transfer
            ))
            .unwrap();
            self.next_transfer = self.next_transfer.wrapping_add(1);
            let fd = unsafe {
                libc::shm_open(
                    name.as_ptr(),
                    libc::O_CREAT | libc::O_EXCL | libc::O_RDWR,
                    0o600,
                )
            };
            if fd < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::AlreadyExists {
                    continue;
                }
                return Err(error);
            }

            let file = unsafe { File::from_raw_fd(fd) };
            if let Err(error) = file.set_len(payload.len() as u64) {
                drop(file);
                unsafe { libc::shm_unlink(name.as_ptr()) };
                return Err(error);
            }
            let mapping = unsafe {
                libc::mmap(
                    std::ptr::null_mut(),
                    payload.len(),
                    libc::PROT_READ | libc::PROT_WRITE,
                    libc::MAP_SHARED,
                    file.as_raw_fd(),
                    0,
                )
            };
            if mapping == libc::MAP_FAILED {
                let error = io::Error::last_os_error();
                drop(file);
                unsafe { libc::shm_unlink(name.as_ptr()) };
                return Err(error);
            }
            unsafe {
                std::ptr::copy_nonoverlapping(payload.as_ptr(), mapping.cast(), payload.len());
                libc::msync(mapping, payload.len(), libc::MS_SYNC);
                libc::munmap(mapping, payload.len());
            }
            return Ok(name);
        }
    }

    fn write_temporary_payload(&mut self, payload: &[u8]) -> io::Result<PathBuf> {
        loop {
            let path = std::env::temp_dir().join(format!(
                "termx-tty-graphics-protocol-{}-{}",
                std::process::id(),
                self.next_transfer
            ));
            self.next_transfer = self.next_transfer.wrapping_add(1);
            match OpenOptions::new().write(true).create_new(true).open(&path) {
                Ok(mut file) => {
                    file.write_all(payload)?;
                    file.flush()?;
                    return Ok(path);
                }
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
    }
}

fn plan_damage(
    screen: ScreenGeometry,
    mut rects: Vec<DamageRect>,
    force_full: bool,
) -> Vec<DamageRect> {
    let full = DamageRect {
        x: 0,
        y: 0,
        width: screen.width,
        height: screen.height,
    };
    if force_full {
        return vec![full];
    }
    rects.retain(|rect| rect.width != 0 && rect.height != 0);

    let mut changed = true;
    while changed {
        changed = false;
        'outer: for left in 0..rects.len() {
            for right in left + 1..rects.len() {
                if rectangles_merge_efficiently(rects[left], rects[right]) {
                    rects[left] = rectangle_union(rects[left], rects[right]);
                    rects.swap_remove(right);
                    changed = true;
                    break 'outer;
                }
            }
        }
    }

    let screen_area = u64::from(screen.width) * u64::from(screen.height);
    let damaged_area: u64 = rects
        .iter()
        .map(|rect| u64::from(rect.width) * u64::from(rect.height))
        .sum();
    if damaged_area.saturating_mul(2) >= screen_area {
        return vec![full];
    }
    if rects.len() > 32 {
        let bounds = rects
            .iter()
            .copied()
            .reduce(rectangle_union)
            .unwrap_or(full);
        let bounds_area = u64::from(bounds.width) * u64::from(bounds.height);
        if bounds_area <= damaged_area.saturating_mul(2) {
            return vec![bounds];
        }
    }
    rects
}

fn rectangles_merge_efficiently(a: DamageRect, b: DamageRect) -> bool {
    let ax2 = i64::from(a.x) + i64::from(a.width);
    let ay2 = i64::from(a.y) + i64::from(a.height);
    let bx2 = i64::from(b.x) + i64::from(b.width);
    let by2 = i64::from(b.y) + i64::from(b.height);
    if i64::from(a.x) > bx2 || i64::from(b.x) > ax2 || i64::from(a.y) > by2 || i64::from(b.y) > ay2
    {
        return false;
    }
    let union = rectangle_union(a, b);
    let union_area = u64::from(union.width) * u64::from(union.height);
    let separate_area =
        u64::from(a.width) * u64::from(a.height) + u64::from(b.width) * u64::from(b.height);
    union_area.saturating_mul(4) <= separate_area.saturating_mul(5)
}

fn rectangle_union(a: DamageRect, b: DamageRect) -> DamageRect {
    let x1 = a.x.min(b.x);
    let y1 = a.y.min(b.y);
    let x2 = (i64::from(a.x) + i64::from(a.width)).max(i64::from(b.x) + i64::from(b.width));
    let y2 = (i64::from(a.y) + i64::from(a.height)).max(i64::from(b.y) + i64::from(b.height));
    DamageRect {
        x: x1,
        y: y1,
        width: (x2 - i64::from(x1)) as u32,
        height: (y2 - i64::from(y1)) as u32,
    }
}

struct TerminalGuard {
    keyboard_enhancement: bool,
    image_id: Cell<Option<u32>>,
}

impl TerminalGuard {
    fn enter() -> io::Result<Self> {
        enable_raw_mode()?;
        let keyboard_enhancement = matches!(terminal::supports_keyboard_enhancement(), Ok(true));
        let mut stdout = io::stdout();
        execute!(
            stdout,
            EnterAlternateScreen,
            Hide,
            EnableMouseCapture,
            EnableFocusChange
        )?;
        // Request SGR-Pixels mouse coordinates when the terminal supports them.
        stdout.write_all(b"\x1b[?1016h")?;
        stdout.flush()?;
        if keyboard_enhancement {
            execute!(
                stdout,
                PushKeyboardEnhancementFlags(
                    KeyboardEnhancementFlags::DISAMBIGUATE_ESCAPE_CODES
                        | KeyboardEnhancementFlags::REPORT_ALL_KEYS_AS_ESCAPE_CODES
                        | KeyboardEnhancementFlags::REPORT_ALTERNATE_KEYS
                        | KeyboardEnhancementFlags::REPORT_EVENT_TYPES
                )
            )?;
        }
        Ok(Self {
            keyboard_enhancement,
            image_id: Cell::new(None),
        })
    }
}

impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let mut stdout = io::stdout();
        if let Some(image_id) = self.image_id.get() {
            let _ = delete_kitty_image(&mut stdout, image_id);
        }
        let _ = stdout.write_all(b"\x1b[?1016l");
        if self.keyboard_enhancement {
            let _ = execute!(stdout, PopKeyboardEnhancementFlags);
        }
        let _ = execute!(
            stdout,
            DisableFocusChange,
            DisableMouseCapture,
            Show,
            LeaveAlternateScreen
        );
        let _ = disable_raw_mode();
    }
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct FileConfig {
    display: Option<u16>,
    socket: Option<PathBuf>,
    log: Option<PathBuf>,
    trace_directory: Option<PathBuf>,
    dpi: Option<f64>,
    command: Option<Vec<String>>,
}

fn default_config_path() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|path| !path.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .map(|directory| directory.join("termx/config.toml"))
}

fn help_requested(arguments: &[String]) -> bool {
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            "-h" | "--help" => return true,
            "--" => return false,
            "--config" | "--display" | "--socket" | "--log" | "--trace-directory" | "--dpi" => {
                index += 2
            }
            argument if !argument.starts_with('-') => return false,
            _ => index += 1,
        }
    }
    false
}

fn extract_config_argument(arguments: Vec<String>) -> io::Result<(Option<PathBuf>, Vec<String>)> {
    let mut config = None;
    let mut filtered = Vec::with_capacity(arguments.len());
    let mut arguments = arguments.into_iter();

    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--" => {
                filtered.push(argument);
                filtered.extend(arguments);
                break;
            }
            "--config" => {
                if config.is_some() {
                    return Err(invalid_input("--config may only be specified once"));
                }
                config = Some(PathBuf::from(
                    arguments
                        .next()
                        .ok_or_else(|| invalid_input("--config requires a path"))?,
                ));
            }
            "--display" | "--socket" | "--log" | "--trace-directory" | "--dpi" => {
                filtered.push(argument);
                if let Some(value) = arguments.next() {
                    filtered.push(value);
                }
            }
            _ if !argument.starts_with('-') => {
                filtered.push(argument);
                filtered.extend(arguments);
                break;
            }
            _ => filtered.push(argument),
        }
    }
    Ok((config, filtered))
}

fn load_file_config(path: Option<PathBuf>, required: bool) -> io::Result<FileConfig> {
    let Some(path) = path else {
        return Ok(FileConfig::default());
    };
    if !required && !path.exists() {
        return Ok(FileConfig::default());
    }
    let contents = fs::read_to_string(&path).map_err(|error| {
        io::Error::new(
            error.kind(),
            format!("could not read {}: {error}", path.display()),
        )
    })?;
    toml::from_str(&contents).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("could not parse {}: {error}", path.display()),
        )
    })
}

#[derive(Clone, Debug, PartialEq)]
struct Options {
    display: u16,
    socket: PathBuf,
    lock: PathBuf,
    log: PathBuf,
    trace_directory: Option<PathBuf>,
    dpi: Option<f64>,
    command: Option<Vec<String>>,
}

impl Options {
    fn load() -> io::Result<Option<Self>> {
        let arguments: Vec<_> = std::env::args().skip(1).collect();
        if help_requested(&arguments) {
            return Ok(None);
        }
        let (config_argument, arguments) = extract_config_argument(arguments)?;
        let config_environment = std::env::var_os("TERMX_CONFIG").map(PathBuf::from);
        let config_required = config_argument.is_some() || config_environment.is_some();
        let config_path = config_argument
            .or(config_environment)
            .or_else(default_config_path);
        let config = load_file_config(config_path, config_required)?;
        let display_environment = std::env::var("TINYX_DISPLAY").ok();
        let socket_environment = std::env::var_os("TINYX_X11_SOCKET").map(PathBuf::from);
        let log_environment = std::env::var_os("TERMX_LOG").map(PathBuf::from);
        let trace_environment = std::env::var_os("TERMX_TRACE_DIRECTORY").map(PathBuf::from);
        let dpi_environment = std::env::var("TINYX_DPI").ok();
        Self::parse(
            arguments,
            display_environment.as_deref(),
            socket_environment,
            log_environment,
            trace_environment,
            dpi_environment.as_deref(),
            config,
        )
        .map(Some)
    }

    fn parse(
        arguments: impl IntoIterator<Item = String>,
        display_environment: Option<&str>,
        socket_environment: Option<PathBuf>,
        log_environment: Option<PathBuf>,
        trace_environment: Option<PathBuf>,
        dpi_environment: Option<&str>,
        config: FileConfig,
    ) -> io::Result<Self> {
        let mut display = display_environment
            .map(str::parse)
            .transpose()
            .map_err(|_| invalid_input("TINYX_DISPLAY must be an unsigned display number"))?
            .or(config.display)
            .unwrap_or(99);
        let mut socket = socket_environment.or(config.socket);
        let mut log = log_environment.or(config.log);
        let mut trace_directory = trace_environment.or(config.trace_directory);
        let mut dpi = dpi_environment
            .map(str::parse::<f64>)
            .transpose()
            .map_err(|_| invalid_input("TINYX_DPI must be a number"))?
            .or(config.dpi);
        if dpi.is_some_and(|value| !valid_dpi(value)) {
            return Err(invalid_input("TINYX_DPI must be between 1 and 1000"));
        }
        let mut command = config.command;
        if command.as_ref().is_some_and(Vec::is_empty) {
            return Err(invalid_input("configuration command must not be empty"));
        }
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            match argument.as_str() {
                "--display" => {
                    display = arguments
                        .next()
                        .ok_or_else(|| invalid_input("--display requires a number"))?
                        .parse()
                        .map_err(|_| invalid_input("--display requires an unsigned number"))?;
                }
                "--socket" => {
                    socket = Some(PathBuf::from(
                        arguments
                            .next()
                            .ok_or_else(|| invalid_input("--socket requires a path"))?,
                    ));
                }
                "--log" => {
                    log = Some(PathBuf::from(
                        arguments
                            .next()
                            .ok_or_else(|| invalid_input("--log requires a path"))?,
                    ));
                }
                "--trace-directory" => {
                    trace_directory =
                        Some(PathBuf::from(arguments.next().ok_or_else(|| {
                            invalid_input("--trace-directory requires a path")
                        })?));
                }
                "--dpi" => {
                    dpi = Some(
                        arguments
                            .next()
                            .ok_or_else(|| invalid_input("--dpi requires a number"))?
                            .parse()
                            .map_err(|_| invalid_input("--dpi requires a number"))?,
                    );
                    if dpi.is_some_and(|value| !valid_dpi(value)) {
                        return Err(invalid_input("--dpi must be between 1 and 1000"));
                    }
                }
                "--" => {
                    let values: Vec<_> = arguments.collect();
                    if values.is_empty() {
                        return Err(invalid_input("-- requires a command"));
                    }
                    command = Some(values);
                    break;
                }
                _ if !argument.starts_with('-') => {
                    let mut values = vec![argument];
                    values.extend(arguments);
                    command = Some(values);
                    break;
                }
                _ => return Err(invalid_input(format!("unknown argument: {argument}"))),
            }
        }
        let custom_socket = socket.is_some();
        if custom_socket && command.is_some() {
            return Err(invalid_input(
                "a launched command requires the standard display socket",
            ));
        }
        let socket = socket.unwrap_or_else(|| PathBuf::from(format!("/tmp/.X11-unix/X{display}")));
        let lock = if custom_socket {
            let mut value = socket.as_os_str().to_os_string();
            value.push(".lock");
            PathBuf::from(value)
        } else {
            PathBuf::from(format!("/tmp/.X{display}-lock"))
        };
        let log =
            log.unwrap_or_else(|| PathBuf::from(format!("/tmp/termx-{}.log", std::process::id())));
        Ok(Self {
            display,
            socket,
            lock,
            log,
            trace_directory,
            dpi,
            command,
        })
    }
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

struct DisplaySocket {
    path: PathBuf,
    lock: PathBuf,
    listener: UnixListener,
}

impl DisplaySocket {
    fn bind(path: PathBuf, lock: PathBuf) -> io::Result<Self> {
        let directory = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(directory)?;
        let mut lock_file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&lock)?;
        if let Err(error) = writeln!(lock_file, "{:>10}", std::process::id()) {
            let _ = fs::remove_file(&lock);
            return Err(error);
        }
        if path.exists() {
            fs::remove_file(&path)?;
        }
        let listener = match UnixListener::bind(&path) {
            Ok(listener) => listener,
            Err(error) => {
                let _ = fs::remove_file(&lock);
                return Err(error);
            }
        };
        if let Err(error) = listener.set_nonblocking(true) {
            let _ = fs::remove_file(&path);
            let _ = fs::remove_file(&lock);
            return Err(error);
        }
        Ok(Self {
            path,
            lock,
            listener,
        })
    }
}

impl Drop for DisplaySocket {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
        let _ = fs::remove_file(&self.lock);
    }
}

struct Logger {
    file: fs::File,
    started: Instant,
}

impl Logger {
    fn open(path: &Path) -> io::Result<Self> {
        if let Some(parent) = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::create_dir_all(parent)?;
        }
        Ok(Self {
            file: OpenOptions::new()
                .create(true)
                .truncate(true)
                .write(true)
                .open(path)?,
            started: Instant::now(),
        })
    }

    fn write(&mut self, message: std::fmt::Arguments<'_>) {
        let milliseconds = self.started.elapsed().as_millis();
        let line = format!("[{milliseconds:>8}ms] {message}");
        let _ = writeln!(self.file, "{line}");
        let _ = self.file.flush();
    }
}

struct ClientTrace {
    client_to_server: fs::File,
    server_to_client: fs::File,
}

impl ClientTrace {
    fn create(directory: &Path, id: u64) -> io::Result<Self> {
        let create = |direction| {
            OpenOptions::new()
                .create(true)
                .truncate(true)
                .write(true)
                .open(directory.join(format!("client-{id}-{direction}.bin")))
        };
        Ok(Self {
            client_to_server: create("c2s")?,
            server_to_client: create("s2c")?,
        })
    }

    fn write_client_to_server(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.client_to_server.write_all(bytes)?;
        self.client_to_server.flush()
    }

    fn write_server_to_client(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.server_to_client.write_all(bytes)?;
        self.server_to_client.flush()
    }
}

struct Connection {
    client: ffi::Client,
    stream: UnixStream,
    inbound: VecDeque<u8>,
    outbound: VecDeque<u8>,
    trace: Option<ClientTrace>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments: Vec<_> = std::env::args().skip(1).collect();
    if arguments
        .first()
        .is_some_and(|arg| arg == "--benchmark-kitty")
    {
        if arguments.len() > 2 {
            return Err("--benchmark-kitty accepts at most one output path".into());
        }
        let output = arguments.get(1).map(PathBuf::from).unwrap_or_else(|| {
            PathBuf::from(format!(
                "/tmp/termx-kitty-benchmark-{}.csv",
                std::process::id()
            ))
        });
        benchmark::run(&output)?;
        eprintln!("benchmark CSV: {}", output.display());
        return Ok(());
    }
    let Some(options) = Options::load()? else {
        print!("{HELP}");
        return Ok(());
    };
    let socket = DisplaySocket::bind(options.socket.clone(), options.lock)?;
    let mut logger = Logger::open(&options.log)?;
    let trace_directory = if let Some(root) = options.trace_directory.as_ref() {
        let directory = root.join(format!("run-{}", std::process::id()));
        fs::create_dir_all(&directory)?;
        eprintln!("trace directory: {}", directory.display());
        logger.write(format_args!(
            "capturing X11 byte streams in {}",
            directory.display()
        ));
        Some(directory)
    } else {
        None
    };
    eprintln!(
        "termx listening on {} (DISPLAY=:{} for the standard path)",
        options.socket.display(),
        options.display
    );
    eprintln!("warning: development host trusts all local clients");
    eprintln!("log file: {}", options.log.display());

    logger.write(format_args!(
        "starting display={} socket={}",
        options.display,
        options.socket.display()
    ));

    let mut screen = ScreenGeometry::for_viewport(Viewport::current());
    let dpi = options
        .dpi
        .map(ScreenDpi::uniform)
        .or_else(detect_terminal_dpi)
        .unwrap_or(ScreenDpi::DEFAULT);
    let (width_mm, height_mm) = dpi.physical_size(screen);
    logger.write(format_args!(
        "configured {}x{} X screen at {:.1}x{:.1} DPI ({}x{} mm)",
        screen.width, screen.height, dpi.x, dpi.y, width_mm, height_mm
    ));
    let server = Server::create(screen.width, screen.height, width_mm, height_mm)?;
    let terminal = TerminalGuard::enter()?;
    let terminal_events = terminal_event_reader();
    let mut session = if let Some(command) = options.command.as_ref() {
        let child = Command::new(&command[0])
            .args(&command[1..])
            .env("DISPLAY", format!(":{}", options.display))
            .spawn()?;
        logger.write(format_args!(
            "launched session command pid={} display=:{} command={command:?}",
            child.id(),
            options.display
        ));
        Some(child)
    } else {
        None
    };
    let mut stdout = io::stdout();
    let mut clients = HashMap::<u64, Connection>::new();
    let mut next_client = 1_u64;
    let mut keyboard = KeyboardState::default();
    let mut pressed_buttons = HashSet::new();
    let mut full_frame_needed = true;
    let mut presenter = KittyPresenter::new();
    terminal.image_id.set(Some(presenter.image_id));
    let mut next_frame = Instant::now();

    loop {
        if let Some(child) = session.as_mut() {
            if let Some(status) = child.try_wait()? {
                logger.write(format_args!("session command exited with {status}"));
                return Ok(());
            }
        }
        loop {
            match socket.listener.accept() {
                Ok((stream, _)) => {
                    stream.set_nonblocking(true)?;
                    let id = next_client;
                    next_client += 1;
                    clients.insert(
                        id,
                        Connection {
                            client: server.open_client()?,
                            stream,
                            inbound: VecDeque::new(),
                            outbound: VecDeque::new(),
                            trace: trace_directory
                                .as_ref()
                                .map(|directory| ClientTrace::create(directory, id))
                                .transpose()?,
                        },
                    );
                    logger.write(format_args!("accepted client {id}"));
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) => return Err(error.into()),
            }
        }

        let mut disconnected = Vec::new();
        for (&id, connection) in &mut clients {
            let mut bytes = [0_u8; 8192];
            loop {
                match connection.stream.read(&mut bytes) {
                    Ok(0) => {
                        connection.client.shutdown_send();
                        disconnected.push(id);
                        break;
                    }
                    Ok(count) => {
                        if let Some(trace) = connection.trace.as_mut() {
                            trace.write_client_to_server(&bytes[..count])?;
                        }
                        connection.inbound.extend(&bytes[..count]);
                        logger.write(format_args!("client {id} read {count} bytes"));
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                    Err(_) => {
                        disconnected.push(id);
                        break;
                    }
                }
            }
            while !connection.inbound.is_empty() {
                let (first, second) = connection.inbound.as_slices();
                let source = if first.is_empty() { second } else { first };
                match connection.client.send(source) {
                    Ok(0) => break,
                    Ok(count) => {
                        connection.inbound.drain(..count);
                        logger.write(format_args!("client {id} accepted {count} input bytes"));
                    }
                    Err(ERROR_WOULD_BLOCK) => break,
                    Err(_) => {
                        disconnected.push(id);
                        break;
                    }
                }
            }
        }

        let drive = server.step(256)?;
        if drive.requests_processed != 0 {
            logger.write(format_args!(
                "server dispatched {} request(s), immediate_work={}",
                drive.requests_processed, drive.immediate_work
            ));
        }
        if drive.generation_finished != 0 {
            return Ok(());
        }

        for (&id, connection) in &mut clients {
            if connection.outbound.is_empty() {
                let mut bytes = [0_u8; 8192];
                match connection.client.receive(&mut bytes) {
                    Ok(count) => {
                        if let Some(trace) = connection.trace.as_mut() {
                            trace.write_server_to_client(&bytes[..count])?;
                        }
                        connection.outbound.extend(&bytes[..count]);
                    }
                    Err(ERROR_WOULD_BLOCK) => {}
                    Err(ERROR_CLOSED) => disconnected.push(id),
                    Err(_) => disconnected.push(id),
                }
            }
            while !connection.outbound.is_empty() {
                let (first, second) = connection.outbound.as_slices();
                let source = if first.is_empty() { second } else { first };
                match connection.stream.write(source) {
                    Ok(0) => break,
                    Ok(count) => {
                        connection.outbound.drain(..count);
                        logger.write(format_args!("client {id} wrote {count} bytes"));
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                    Err(_) => {
                        disconnected.push(id);
                        break;
                    }
                }
            }
            if connection.client.is_closed() && connection.outbound.is_empty() {
                disconnected.push(id);
            }
        }

        disconnected.sort_unstable();
        disconnected.dedup();
        for id in disconnected {
            if clients.remove(&id).is_some() {
                logger.write(format_args!("disconnected client {id}"));
            }
        }

        loop {
            let terminal_event = match terminal_events.try_recv() {
                Ok(event) => event?,
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    return Err("terminal event reader stopped".into());
                }
            };
            logger.write(format_args!("terminal event: {terminal_event:?}"));
            match terminal_event {
                Event::Key(key)
                    if key.kind != KeyEventKind::Release
                        && key.code == KeyCode::Char('c')
                        && key.modifiers.contains(KeyModifiers::CONTROL) =>
                {
                    return Ok(());
                }
                Event::Key(key) => {
                    apply_key_event(&server, &mut keyboard, key)?;
                    // Legacy terminals report only presses. Pulse the key so it
                    // cannot remain stuck in the embedded X server.
                    if !terminal.keyboard_enhancement && key.kind == KeyEventKind::Press {
                        let release =
                            KeyEvent::new_with_kind(key.code, key.modifiers, KeyEventKind::Release);
                        apply_key_event(&server, &mut keyboard, release)?;
                    }
                }
                Event::Mouse(mouse) => {
                    let Some((x, y)) =
                        pointer_position(mouse.column, mouse.row, Viewport::current(), screen)
                    else {
                        continue;
                    };
                    match mouse.kind {
                        MouseEventKind::Moved | MouseEventKind::Drag(_) => {
                            server.pointer_motion(x, y)?;
                        }
                        MouseEventKind::Down(button) => {
                            server.pointer_motion(x, y)?;
                            if let Some(button) = x_button(button) {
                                if pressed_buttons.insert(button) {
                                    server.pointer_button(button, true)?;
                                }
                            }
                        }
                        MouseEventKind::Up(button) => {
                            server.pointer_motion(x, y)?;
                            if let Some(button) = x_button(button) {
                                pressed_buttons.remove(&button);
                                server.pointer_button(button, false)?;
                            }
                        }
                        MouseEventKind::ScrollUp => {
                            server.pointer_button(4, true)?;
                            server.pointer_button(4, false)?;
                        }
                        MouseEventKind::ScrollDown => {
                            server.pointer_button(5, true)?;
                            server.pointer_button(5, false)?;
                        }
                        _ => {}
                    }
                }
                Event::FocusLost => {
                    server.release_all_keys()?;
                    keyboard.clear();
                    for button in pressed_buttons.drain() {
                        server.pointer_button(button, false)?;
                    }
                }
                Event::Resize(columns, rows) => {
                    let resized = ScreenGeometry::for_viewport(Viewport::new(columns, rows));
                    if resized != screen {
                        server.resize(resized.width, resized.height)?;
                        screen = resized;
                        logger.write(format_args!(
                            "resized X screen to {}x{} for terminal pixel area",
                            screen.width, screen.height
                        ));
                    }
                    full_frame_needed = true;
                }
                _ => {}
            }
        }

        if Instant::now() >= next_frame {
            let damage = server.take_damage()?;
            if full_frame_needed || !damage.is_empty() {
                let viewport = Viewport::current();
                let stats = presenter.present(
                    &mut stdout,
                    &server,
                    screen,
                    ImagePlacement::current(viewport, screen),
                    damage,
                    full_frame_needed,
                )?;
                terminal.image_id.set(Some(presenter.image_id));
                logger.write(format_args!(
                    "presented {} region(s), {} pixels, {} payload bytes, medium={}, full_frame={}",
                    stats.regions,
                    stats.pixels,
                    stats.wire_bytes,
                    presenter.last_medium,
                    stats.full_frame
                ));
                full_frame_needed = false;
            }
            next_frame = Instant::now() + FRAME_INTERVAL;
        }

        thread::sleep(Duration::from_millis(2));
    }
}

fn terminal_event_reader() -> mpsc::Receiver<io::Result<Event>> {
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        loop {
            let event = event::read();
            let failed = event.is_err();
            if sender.send(event).is_err() || failed {
                break;
            }
        }
    });
    receiver
}

fn apply_key_event(
    server: &Server,
    keyboard: &mut KeyboardState,
    event: KeyEvent,
) -> Result<(), String> {
    for transition in keyboard.handle(event) {
        server.key(transition.keycode, transition.pressed)?;
    }
    Ok(())
}

fn select_kitty_frame(writer: &mut impl Write, image_id: u32, frame: u32) -> io::Result<()> {
    write!(writer, "\x1b_Ga=a,q=2,c={frame},i={image_id};\x1b\\")
}

fn delete_kitty_image(writer: &mut impl Write, image_id: u32) -> io::Result<()> {
    write!(writer, "\x1b_Ga=d,d=I,i={image_id},q=2;\x1b\\")?;
    writer.flush()
}

fn pointer_position(
    column: u16,
    row: u16,
    viewport: Viewport,
    screen: ScreenGeometry,
) -> Option<(i32, i32)> {
    let placement = ImagePlacement::current(viewport, screen);
    let (x, y) = if let Some((window_width, window_height)) = placement.pixel_size {
        let total_rows = u32::from(viewport.image_rows);
        let left = u32::from(placement.column) * window_width / u32::from(viewport.columns);
        let top = u32::from(placement.row) * window_height / total_rows;
        let width = u32::from(placement.columns) * window_width / u32::from(viewport.columns);
        let height = u32::from(placement.rows) * window_height / total_rows;
        let column = u32::from(column);
        let row = u32::from(row);
        if column < left || column >= left + width || row < top || row >= top + height {
            return None;
        }
        (
            (column - left) * screen.width / width.max(1),
            (row - top) * screen.height / height.max(1),
        )
    } else {
        if column < placement.column
            || column >= placement.column + placement.columns
            || row < placement.row
            || row >= placement.row + placement.rows
        {
            return None;
        }
        (
            u32::from(column - placement.column) * screen.width / u32::from(placement.columns),
            u32::from(row - placement.row) * screen.height / u32::from(placement.rows),
        )
    };
    Some((
        x.min(screen.width - 1) as i32,
        y.min(screen.height - 1) as i32,
    ))
}

fn x_button(button: MouseButton) -> Option<u32> {
    match button {
        MouseButton::Left => Some(1),
        MouseButton::Middle => Some(2),
        MouseButton::Right => Some(3),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn help_describes_options_and_environment() {
        assert!(HELP.starts_with("TermX 0.1.0\n"));
        assert!(HELP.contains("Usage: termx [OPTIONS] [-- COMMAND [ARGUMENT...]]"));
        assert!(HELP.contains("--display NUMBER"));
        assert!(HELP.contains("TERMX_LOG"));
        assert!(HELP.contains("--trace-directory PATH"));
        assert!(help_requested(&[String::from("--help")]));
        assert!(!help_requested(&[
            String::from("--"),
            String::from("dwm"),
            String::from("--help"),
        ]));
    }

    #[test]
    fn socket_path_can_be_configured_by_environment_or_command_line() {
        let environment = Options::parse(
            Vec::<String>::new(),
            Some("7"),
            Some(PathBuf::from("/run/user/1000/tinyx.sock")),
            Some(PathBuf::from("/tmp/env.log")),
            None,
            Some("144"),
            FileConfig::default(),
        )
        .unwrap();
        assert_eq!(environment.display, 7);
        assert_eq!(environment.socket, Path::new("/run/user/1000/tinyx.sock"));
        assert_eq!(
            environment.lock,
            Path::new("/run/user/1000/tinyx.sock.lock")
        );
        assert_eq!(environment.log, Path::new("/tmp/env.log"));
        assert_eq!(environment.dpi, Some(144.0));
    }

    #[test]
    fn default_socket_path_follows_display_number() {
        let options = Options::parse(
            Vec::<String>::new(),
            Some("42"),
            None,
            None,
            None,
            None,
            FileConfig::default(),
        )
        .unwrap();
        assert_eq!(options.socket, Path::new("/tmp/.X11-unix/X42"));
        assert_eq!(options.lock, Path::new("/tmp/.X42-lock"));
    }

    #[test]
    fn session_command_and_arguments_are_preserved() {
        let options = Options::parse(
            ["--display", "7", "--", "dwm", "--verbose"].map(String::from),
            None,
            None,
            None,
            None,
            None,
            FileConfig::default(),
        )
        .unwrap();
        assert_eq!(
            options.command,
            Some(vec![String::from("dwm"), String::from("--verbose")])
        );
    }

    #[test]
    fn session_command_rejects_a_custom_socket() {
        let result = Options::parse(
            ["dwm"].map(String::from),
            None,
            Some(PathBuf::from("/tmp/custom.sock")),
            None,
            None,
            None,
            FileConfig::default(),
        );
        assert!(result.is_err());
    }

    #[test]
    fn toml_config_supplies_defaults_below_environment_and_cli() {
        let config: FileConfig = toml::from_str(
            r#"
                display = 3
                log = "/tmp/config.log"
                trace_directory = "/tmp/trace"
                dpi = 96
                command = ["dwm"]
            "#,
        )
        .unwrap();
        let options = Options::parse(
            ["--display", "5"].map(String::from),
            Some("4"),
            None,
            Some(PathBuf::from("/tmp/environment.log")),
            None,
            None,
            config,
        )
        .unwrap();
        assert_eq!(options.display, 5);
        assert_eq!(options.log, Path::new("/tmp/environment.log"));
        assert_eq!(options.trace_directory, Some(PathBuf::from("/tmp/trace")));
        assert_eq!(options.dpi, Some(96.0));
        assert_eq!(options.command, Some(vec![String::from("dwm")]));
    }

    #[test]
    fn config_argument_is_removed_before_option_parsing() {
        let (path, arguments) = extract_config_argument(
            ["--display", "5", "--config", "/tmp/termx.toml", "--", "dwm"]
                .map(String::from)
                .into(),
        )
        .unwrap();
        assert_eq!(path, Some(PathBuf::from("/tmp/termx.toml")));
        assert_eq!(arguments, ["--display", "5", "--", "dwm"].map(String::from));
    }

    #[test]
    fn terminal_dpi_is_parsed_and_converted_to_millimeters() {
        let dpi = parse_terminal_dpi("dpi_x: 144\ndpi_y: 144\n").unwrap();
        assert_eq!(dpi, ScreenDpi::uniform(144.0));
        assert_eq!(
            dpi.physical_size(ScreenGeometry {
                width: 1920,
                height: 1080,
            }),
            (339, 191)
        );
    }

    #[test]
    fn viewport_uses_the_full_terminal() {
        assert_eq!(
            Viewport::new(80, 24),
            Viewport {
                columns: 80,
                image_rows: 24,
            }
        );
    }

    #[test]
    fn kitty_animation_transmission_is_chunked_and_compressed() {
        let mut presenter = KittyPresenter::new();
        presenter.local_media = false;
        let mut output = Vec::new();
        presenter
            .transmit(
                &mut output,
                "a=f,f=24,i=1,q=2,r=2,x=2,y=3,s=100,v=100,X=1",
                &vec![0xa5; 30_000],
                true,
            )
            .unwrap();
        let text = String::from_utf8(output).unwrap();
        assert!(
            text.starts_with("\u{1b}_Ga=f,f=24,i=1,q=2,r=2,x=2,y=3,s=100,v=100,X=1,o=z,t=d,m=")
        );
        assert!(text.ends_with("\u{1b}\\"));
    }

    #[test]
    fn kitty_never_policy_skips_compression() {
        let mut presenter = KittyPresenter::new();
        let pixels = vec![0xa5; 30_000];
        let mut output = Vec::new();
        let bytes = presenter
            .transmit_with(
                &mut output,
                "a=f,f=24,i=1,q=2,r=1,x=0,y=0,s=100,v=100,X=1",
                &pixels,
                true,
                ZlibPolicy::Never,
                GraphicsTransport::Direct,
                4096,
            )
            .unwrap();
        assert_eq!(bytes, pixels.len());
        assert!(!String::from_utf8(output).unwrap().contains(",o=z"));
    }

    #[test]
    fn kitty_adaptive_policy_skips_compression_for_shared_memory() {
        let mut presenter = KittyPresenter::new();
        let pixels = vec![0xa5; 30_000];
        let mut output = Vec::new();
        let bytes = presenter
            .transmit_with(
                &mut output,
                "a=f,f=24,i=1,q=2,r=1,x=0,y=0,s=100,v=100,X=1",
                &pixels,
                true,
                ZlibPolicy::Adaptive,
                GraphicsTransport::SharedMemory,
                4096,
            )
            .unwrap();
        assert_eq!(bytes, pixels.len());
        let text = String::from_utf8(output).unwrap();
        assert!(!text.contains(",o=z"));
        let encoded_name = text
            .split_once(';')
            .unwrap()
            .1
            .strip_suffix("\u{1b}\\")
            .unwrap();
        let name = base64::engine::general_purpose::STANDARD
            .decode(encoded_name)
            .unwrap();
        let name = CString::new(name).unwrap();
        assert_eq!(unsafe { libc::shm_unlink(name.as_ptr()) }, 0);
    }

    #[test]
    fn kitty_auto_policy_uses_shared_memory_for_small_local_updates() {
        let mut presenter = KittyPresenter::new();
        presenter.local_media = true;
        let pixels = vec![0xa5; 768];
        let mut output = Vec::new();
        let bytes = presenter
            .transmit_with(
                &mut output,
                "a=f,f=24,i=1,q=2,r=1,x=0,y=0,s=16,v=16,X=1",
                &pixels,
                true,
                ZlibPolicy::Adaptive,
                GraphicsTransport::Auto,
                4096,
            )
            .unwrap();
        assert_eq!(bytes, pixels.len());
        assert_eq!(presenter.last_medium, "shared-memory");
        let text = String::from_utf8(output).unwrap();
        assert!(!text.contains(",o=z"));
        let encoded_name = text
            .split_once(';')
            .unwrap()
            .1
            .strip_suffix("\u{1b}\\")
            .unwrap();
        let name = base64::engine::general_purpose::STANDARD
            .decode(encoded_name)
            .unwrap();
        let name = CString::new(name).unwrap();
        assert_eq!(unsafe { libc::shm_unlink(name.as_ptr()) }, 0);
    }

    #[test]
    fn kitty_frame_selection_has_an_empty_payload_separator() {
        let mut output = Vec::new();
        select_kitty_frame(&mut output, 7, 1).unwrap();
        assert_eq!(output, b"\x1b_Ga=a,q=2,c=1,i=7;\x1b\\");
    }

    #[test]
    fn shared_memory_payload_can_be_reopened() {
        let mut presenter = KittyPresenter::new();
        let payload = b"shared framebuffer bytes";
        let name = presenter.write_shared_payload(payload).unwrap();
        let fd = unsafe { libc::shm_open(name.as_ptr(), libc::O_RDONLY, 0) };
        assert!(fd >= 0);
        let file = unsafe { File::from_raw_fd(fd) };
        let mapping = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                payload.len(),
                libc::PROT_READ,
                libc::MAP_SHARED,
                file.as_raw_fd(),
                0,
            )
        };
        assert_ne!(mapping, libc::MAP_FAILED);
        let actual = unsafe { std::slice::from_raw_parts(mapping.cast::<u8>(), payload.len()) };
        assert_eq!(actual, payload);
        unsafe { libc::munmap(mapping, payload.len()) };
        drop(file);
        assert_eq!(unsafe { libc::shm_unlink(name.as_ptr()) }, 0);
    }

    #[test]
    fn damage_is_coalesced_and_large_updates_become_full_frames() {
        let screen = ScreenGeometry {
            width: 100,
            height: 100,
        };
        assert_eq!(
            plan_damage(
                screen,
                vec![
                    DamageRect {
                        x: 1,
                        y: 1,
                        width: 4,
                        height: 4
                    },
                    DamageRect {
                        x: 5,
                        y: 1,
                        width: 4,
                        height: 4
                    },
                ],
                false,
            ),
            vec![DamageRect {
                x: 1,
                y: 1,
                width: 8,
                height: 4
            }]
        );
        assert_eq!(
            plan_damage(
                screen,
                vec![DamageRect {
                    x: 0,
                    y: 0,
                    width: 80,
                    height: 80
                }],
                false,
            ),
            vec![DamageRect {
                x: 0,
                y: 0,
                width: 100,
                height: 100
            }]
        );
    }
}
