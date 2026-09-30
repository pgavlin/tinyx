#![cfg(unix)]

mod ffi;
mod input;

use std::cell::Cell;
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
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
use ffi::{ERROR_CLOSED, ERROR_WOULD_BLOCK, Server};
use input::KeyboardState;

const FRAME_INTERVAL: Duration = Duration::from_millis(33);
const LOG_INTERVAL: Duration = Duration::from_millis(100);
const FALLBACK_CELL_WIDTH: u32 = 8;
const FALLBACK_CELL_HEIGHT: u32 = 16;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Viewport {
    columns: u16,
    image_rows: u16,
    log_rows: u16,
}

impl Viewport {
    fn current() -> Self {
        let (columns, rows) = terminal::size().unwrap_or((80, 24));
        Self::new(columns, rows)
    }

    fn new(columns: u16, rows: u16) -> Self {
        let columns = columns.max(1);
        let rows = rows.max(2);
        let log_rows = (rows / 3).clamp(1, 6).min(rows - 1);
        Self {
            columns,
            image_rows: rows - log_rows,
            log_rows,
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
        let total_rows = u32::from(viewport.image_rows + viewport.log_rows);
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
            u32::from(viewport.image_rows + viewport.log_rows) * 2,
        ));
        let total_rows = u32::from(viewport.image_rows + viewport.log_rows);
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

#[derive(Clone, Debug, Eq, PartialEq)]
struct Options {
    display: u16,
    socket: PathBuf,
    lock: PathBuf,
    log: PathBuf,
}

impl Options {
    fn load() -> io::Result<Self> {
        let display_environment = std::env::var("TINYX_DISPLAY").ok();
        let socket_environment = std::env::var_os("TINYX_X11_SOCKET").map(PathBuf::from);
        let log_environment = std::env::var_os("TINYX_KITTY_LOG").map(PathBuf::from);
        Self::parse(
            std::env::args().skip(1),
            display_environment.as_deref(),
            socket_environment,
            log_environment,
        )
    }

    fn parse(
        arguments: impl IntoIterator<Item = String>,
        display_environment: Option<&str>,
        socket_environment: Option<PathBuf>,
        log_environment: Option<PathBuf>,
    ) -> io::Result<Self> {
        let mut display = display_environment
            .map(str::parse)
            .transpose()
            .map_err(|_| invalid_input("TINYX_DISPLAY must be an unsigned display number"))?
            .unwrap_or(99);
        let mut socket = socket_environment;
        let mut log = log_environment;
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
                _ => return Err(invalid_input(format!("unknown argument: {argument}"))),
            }
        }
        let custom_socket = socket.is_some();
        let socket = socket.unwrap_or_else(|| PathBuf::from(format!("/tmp/.X11-unix/X{display}")));
        let lock = if custom_socket {
            let mut value = socket.as_os_str().to_os_string();
            value.push(".lock");
            PathBuf::from(value)
        } else {
            PathBuf::from(format!("/tmp/.X{display}-lock"))
        };
        let log = log.unwrap_or_else(|| {
            PathBuf::from(format!("/tmp/tinyx-kitty-{}.log", std::process::id()))
        });
        Ok(Self {
            display,
            socket,
            lock,
            log,
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
    lines: VecDeque<String>,
    dirty: bool,
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
            lines: VecDeque::new(),
            dirty: true,
        })
    }

    fn write(&mut self, message: std::fmt::Arguments<'_>) {
        let milliseconds = self.started.elapsed().as_millis();
        let line = format!("[{milliseconds:>8}ms] {message}");
        let _ = writeln!(self.file, "{line}");
        let _ = self.file.flush();
        self.lines.push_back(line);
        while self.lines.len() > 256 {
            self.lines.pop_front();
        }
        self.dirty = true;
    }

    fn render_overlay(&mut self, writer: &mut impl Write, viewport: Viewport) -> io::Result<()> {
        if !self.dirty {
            return Ok(());
        }
        writer.write_all(b"\x1b[s")?;
        let first_row = viewport.image_rows + 1;
        let visible = usize::from(viewport.log_rows);
        let skip = self.lines.len().saturating_sub(visible);
        for (index, line) in self.lines.iter().skip(skip).enumerate() {
            let row = first_row + index as u16;
            write!(writer, "\x1b[{row};1H\x1b[2K")?;
            writer.write_all(visible_text(line, viewport.columns).as_bytes())?;
        }
        for index in self.lines.len().min(visible)..visible {
            let row = first_row + index as u16;
            write!(writer, "\x1b[{row};1H\x1b[2K")?;
        }
        writer.write_all(b"\x1b[u")?;
        writer.flush()?;
        self.dirty = false;
        Ok(())
    }
}

fn visible_text(value: &str, columns: u16) -> String {
    value
        .chars()
        .map(|character| {
            if character.is_control() {
                ' '
            } else {
                character
            }
        })
        .take(usize::from(columns))
        .collect()
}

struct Connection {
    client: ffi::Client,
    stream: UnixStream,
    inbound: VecDeque<u8>,
    outbound: VecDeque<u8>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let options = Options::load()?;
    let socket = DisplaySocket::bind(options.socket.clone(), options.lock)?;
    let mut logger = Logger::open(&options.log)?;
    eprintln!(
        "tinyx-kitty listening on {} (DISPLAY=:{} for the standard path)",
        options.socket.display(),
        options.display
    );
    eprintln!("warning: development host trusts all local clients");
    eprintln!("live log: tail -f {}", options.log.display());

    logger.write(format_args!(
        "starting display={} socket={}",
        options.display,
        options.socket.display()
    ));

    let screen = ScreenGeometry::for_viewport(Viewport::current());
    logger.write(format_args!(
        "configured {}x{} X screen for terminal pixel area",
        screen.width, screen.height
    ));
    let server = Server::create(screen.width, screen.height)?;
    let terminal = TerminalGuard::enter()?;
    let mut stdout = io::stdout();
    let mut clients = HashMap::<u64, Connection>::new();
    let mut next_client = 1_u64;
    let mut keyboard = KeyboardState::default();
    let mut pressed_buttons = HashSet::new();
    let mut frame_dirty = true;
    let mut framebuffer_snapshot = Vec::new();
    let mut missed_damage_reported = false;
    let mut current_image_id = None;
    let mut next_image_id = 1_u32;
    let mut next_frame = Instant::now();
    let mut next_log = Instant::now();

    loop {
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
                    Ok(count) => connection.outbound.extend(&bytes[..count]),
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

        while event::poll(Duration::ZERO)? {
            let terminal_event = event::read()?;
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
                Event::Resize(_, _) => {
                    frame_dirty = true;
                    logger.dirty = true;
                }
                _ => {}
            }
        }

        if Instant::now() >= next_frame {
            let damaged = server.take_damage()?;
            let framebuffer_changed =
                server.framebuffer_snapshot_changed(&mut framebuffer_snapshot)?;
            let changed_without_damage = framebuffer_changed && !damaged;
            frame_dirty |= damaged || framebuffer_changed;
            if changed_without_damage && !missed_damage_reported {
                logger.write(format_args!(
                    "framebuffer changed without a Damage notification; enabling snapshot fallback"
                ));
                missed_damage_reported = true;
            }
            if frame_dirty {
                let png = server.encode_png()?;
                let viewport = Viewport::current();
                present_kitty_png(
                    &mut stdout,
                    &png,
                    ImagePlacement::current(viewport, screen),
                    next_image_id,
                    current_image_id,
                )?;
                current_image_id = Some(next_image_id);
                terminal.image_id.set(current_image_id);
                next_image_id = next_image_id.checked_add(1).unwrap_or(1);
                logger.write(format_args!(
                    "presented {}x{} frame as {} PNG bytes",
                    screen.width,
                    screen.height,
                    png.len()
                ));
                frame_dirty = false;
            }
            next_frame = Instant::now() + FRAME_INTERVAL;
        }

        if Instant::now() >= next_log {
            logger.render_overlay(&mut stdout, Viewport::current())?;
            next_log = Instant::now() + LOG_INTERVAL;
        }

        thread::sleep(Duration::from_millis(2));
    }
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

fn present_kitty_png(
    writer: &mut impl Write,
    png: &[u8],
    placement: ImagePlacement,
    image_id: u32,
    previous_image_id: Option<u32>,
) -> io::Result<()> {
    write!(
        writer,
        "\x1b[s\x1b[{};{}H",
        placement.row + 1,
        placement.column + 1
    )?;
    let encoded = base64::engine::general_purpose::STANDARD.encode(png);
    for (index, chunk) in encoded.as_bytes().chunks(4096).enumerate() {
        let more = usize::from((index + 1) * 4096 < encoded.len());
        if index == 0 {
            write!(
                writer,
                "\x1b_Ga=T,f=100,t=d,i={image_id},p=1,q=2,C=1,c={},r={},m={more};",
                placement.columns, placement.rows
            )?;
        } else {
            write!(writer, "\x1b_Gm={more};")?;
        }
        writer.write_all(chunk)?;
        writer.write_all(b"\x1b\\")?;
    }
    // Kitty does not display a chunked transmission until the final chunk has
    // been received and validated. Keep the previous placement visible until
    // that point, then remove it without exposing a blank frame.
    if let Some(previous_image_id) = previous_image_id {
        delete_kitty_image(writer, previous_image_id)?;
    }
    writer.write_all(b"\x1b[u")?;
    writer.flush()
}

fn delete_kitty_image(writer: &mut impl Write, image_id: u32) -> io::Result<()> {
    write!(writer, "\x1b_Ga=d,d=I,i={image_id},q=2\x1b\\")?;
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
        let total_rows = u32::from(viewport.image_rows + viewport.log_rows);
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
    fn socket_path_can_be_configured_by_environment_or_command_line() {
        let environment = Options::parse(
            Vec::<String>::new(),
            Some("7"),
            Some(PathBuf::from("/run/user/1000/tinyx.sock")),
            Some(PathBuf::from("/tmp/env.log")),
        )
        .unwrap();
        assert_eq!(environment.display, 7);
        assert_eq!(environment.socket, Path::new("/run/user/1000/tinyx.sock"));
        assert_eq!(
            environment.lock,
            Path::new("/run/user/1000/tinyx.sock.lock")
        );
        assert_eq!(environment.log, Path::new("/tmp/env.log"));
    }

    #[test]
    fn default_socket_path_follows_display_number() {
        let options = Options::parse(Vec::<String>::new(), Some("42"), None, None).unwrap();
        assert_eq!(options.socket, Path::new("/tmp/.X11-unix/X42"));
        assert_eq!(options.lock, Path::new("/tmp/.X42-lock"));
    }

    #[test]
    fn viewport_reserves_rows_for_a_tailing_log() {
        assert_eq!(
            Viewport::new(80, 24),
            Viewport {
                columns: 80,
                image_rows: 18,
                log_rows: 6,
            }
        );
        assert_eq!(visible_text("ok\nunsafe\x1b", 20), "ok unsafe ");
    }

    #[test]
    fn kitty_png_is_chunked_and_terminated() {
        let mut output = Vec::new();
        present_kitty_png(
            &mut output,
            &vec![0xa5; 4096],
            ImagePlacement::new(
                Viewport::new(80, 24),
                Some((800, 480)),
                ScreenGeometry {
                    width: 640,
                    height: 480,
                },
            ),
            8,
            Some(7),
        )
        .unwrap();
        let text = String::from_utf8(output).unwrap();
        assert!(text.starts_with("\u{1b}[s\u{1b}[1;17H\u{1b}_Ga=T"));
        assert!(text.contains("a=T,f=100,t=d,i=8,p=1,q=2,C=1,c=48,r=18,m=1;"));
        assert!(text.contains("\u{1b}_Gm=0;"));
        let deletion = text.find("a=d,d=I,i=7,q=2").unwrap();
        let final_chunk = text.find("\u{1b}_Gm=0;").unwrap();
        assert!(deletion > final_chunk);
        assert!(text.ends_with("\u{1b}[u"));
    }
}
