use super::*;
use std::os::fd::AsRawFd;

const BASE_SAMPLES: usize = 2;
const DAMAGE_SAMPLES: usize = 4;
const RESPONSE_TIMEOUT_MS: i32 = 5_000;

struct BenchmarkTerminal;

impl BenchmarkTerminal {
    fn enter() -> io::Result<Self> {
        enable_raw_mode()?;
        execute!(io::stdout(), EnterAlternateScreen, Hide)?;
        Ok(Self)
    }
}

impl Drop for BenchmarkTerminal {
    fn drop(&mut self) {
        let _ = execute!(io::stdout(), Show, LeaveAlternateScreen);
        let _ = disable_raw_mode();
    }
}

#[derive(Clone, Copy)]
struct BenchmarkConfig {
    zlib: ZlibPolicy,
    zlib_name: &'static str,
    transport: GraphicsTransport,
    transport_name: &'static str,
    chunk_size: usize,
    chunk_name: &'static str,
}

struct ResultRow {
    screen: ScreenGeometry,
    config: BenchmarkConfig,
    case: &'static str,
    width: u32,
    height: u32,
    payload_bytes: usize,
    medium: &'static str,
    p50: Duration,
    p90: Duration,
}

pub fn run(output: &Path) -> Result<(), Box<dyn std::error::Error>> {
    if std::env::var_os("KITTY_WINDOW_ID").is_none() {
        return Err("the Kitty graphics benchmark must run inside Kitty".into());
    }

    let viewport = Viewport::current();
    let native = ScreenGeometry::for_viewport(viewport);
    let screens = benchmark_screens(native);
    let terminal = BenchmarkTerminal::enter()?;
    let mut stdout = io::stdout().lock();
    let mut presenter = KittyPresenter::new();
    presenter.image_id = std::process::id().max(1);
    let barrier_id = presenter.image_id.checked_add(1).unwrap_or(1);
    let mut results = Vec::new();

    for screen in screens {
        let placement = ImagePlacement::current(viewport, screen);
        let base = benchmark_frame(screen);
        for config in benchmark_configs() {
            let mut samples = Vec::with_capacity(BASE_SAMPLES);
            let mut payload_bytes = 0;
            let mut medium = "direct";
            for _ in 0..BASE_SAMPLES {
                write!(stdout, "\x1b[H")?;
                let start = Instant::now();
                payload_bytes = presenter.transmit_with(
                    &mut stdout,
                    &format!(
                        "a=T,f=24,s={},v={},i={},p=1,q=2,C=1,c={},r={}",
                        screen.width,
                        screen.height,
                        presenter.image_id,
                        placement.columns,
                        placement.rows
                    ),
                    &base,
                    false,
                    config.zlib,
                    config.transport,
                    config.chunk_size,
                )?;
                send_barrier(&mut stdout, barrier_id)?;
                stdout.flush()?;
                wait_for_response(barrier_id)?;
                samples.push(start.elapsed());
                medium = presenter.last_medium;
            }
            let (p50, p90) = percentiles(&mut samples);
            results.push(ResultRow {
                screen,
                config,
                case: "base",
                width: screen.width,
                height: screen.height,
                payload_bytes,
                medium,
                p50,
                p90,
            });

            let cases = [
                (
                    "cursor",
                    16_u32.min(screen.width),
                    16_u32.min(screen.height),
                ),
                (
                    "text-line",
                    512_u32.min(screen.width),
                    32_u32.min(screen.height),
                ),
                (
                    "window",
                    (screen.width / 2).max(1),
                    (screen.height / 2).max(1),
                ),
                ("full", screen.width, screen.height),
            ];
            for (case, width, height) in cases {
                let x = (screen.width - width) / 2;
                let y = (screen.height - height) / 2;
                let mut samples = Vec::with_capacity(DAMAGE_SAMPLES);
                let mut payload_bytes = 0;
                let mut medium = "direct";
                for sample in 0..DAMAGE_SAMPLES {
                    let pixels = damage_frame(width, height, sample as u8);
                    let start = Instant::now();
                    payload_bytes = presenter.transmit_with(
                        &mut stdout,
                        &format!(
                            "a=f,r=1,i={},f=24,q=2,x={x},y={y},s={width},v={height},X=1",
                            presenter.image_id
                        ),
                        &pixels,
                        true,
                        config.zlib,
                        config.transport,
                        config.chunk_size,
                    )?;
                    write!(stdout, "\x1b_Ga=a,q=2,c=1,i={};\x1b\\", presenter.image_id)?;
                    send_barrier(&mut stdout, barrier_id)?;
                    stdout.flush()?;
                    wait_for_response(barrier_id)?;
                    samples.push(start.elapsed());
                    medium = presenter.last_medium;
                }
                let (p50, p90) = percentiles(&mut samples);
                results.push(ResultRow {
                    screen,
                    config,
                    case,
                    width,
                    height,
                    payload_bytes,
                    medium,
                    p50,
                    p90,
                });
            }
            delete_kitty_image(&mut stdout, presenter.image_id)?;
        }
    }

    drop(stdout);
    drop(terminal);

    if let Some(parent) = output
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        fs::create_dir_all(parent)?;
    }
    let mut csv = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(output)?;
    writeln!(csv, "# Kitty graphics transport benchmark")?;
    writeln!(
        csv,
        "# native_terminal_pixels={}x{}",
        native.width, native.height
    )?;
    writeln!(
        csv,
        "screen_width,screen_height,zlib,transport,chunk_size,case,update_width,update_height,payload_bytes,actual_medium,p50_ms,p90_ms"
    )?;
    for result in results {
        writeln!(
            csv,
            "{},{},{},{},{},{},{},{},{},{},{:.3},{:.3}",
            result.screen.width,
            result.screen.height,
            result.config.zlib_name,
            result.config.transport_name,
            result.config.chunk_name,
            result.case,
            result.width,
            result.height,
            result.payload_bytes,
            result.medium,
            result.p50.as_secs_f64() * 1000.0,
            result.p90.as_secs_f64() * 1000.0
        )?;
    }
    csv.flush()?;
    Ok(())
}

fn benchmark_configs() -> Vec<BenchmarkConfig> {
    let mut configs = Vec::new();
    for (zlib, zlib_name) in [
        (ZlibPolicy::Never, "never"),
        (ZlibPolicy::Adaptive, "adaptive"),
        (ZlibPolicy::Always, "always"),
    ] {
        for chunk_size in [1024, 4096] {
            configs.push(BenchmarkConfig {
                zlib,
                zlib_name,
                transport: GraphicsTransport::Direct,
                transport_name: "direct",
                chunk_size,
                chunk_name: if chunk_size == 1024 { "1024" } else { "4096" },
            });
        }
        configs.push(BenchmarkConfig {
            zlib,
            zlib_name,
            transport: GraphicsTransport::TemporaryFile,
            transport_name: "temporary-file",
            chunk_size: 4096,
            chunk_name: "none",
        });
        configs.push(BenchmarkConfig {
            zlib,
            zlib_name,
            transport: GraphicsTransport::SharedMemory,
            transport_name: "shared-memory",
            chunk_size: 4096,
            chunk_name: "none",
        });
    }
    configs
}

fn benchmark_screens(native: ScreenGeometry) -> Vec<ScreenGeometry> {
    let mut screens = vec![
        ScreenGeometry {
            width: 320,
            height: 200,
        },
        ScreenGeometry {
            width: 640,
            height: 400,
        },
        ScreenGeometry {
            width: 1280,
            height: 720,
        },
        ScreenGeometry {
            width: 1920,
            height: 1080,
        },
        native,
    ];
    screens.sort_by_key(|screen| {
        (
            u64::from(screen.width) * u64::from(screen.height),
            screen.width,
        )
    });
    screens.dedup();
    screens
}

fn percentiles(samples: &mut [Duration]) -> (Duration, Duration) {
    samples.sort_unstable();
    (
        samples[samples.len() / 2],
        samples[(samples.len() * 9 / 10).min(samples.len() - 1)],
    )
}

fn benchmark_frame(screen: ScreenGeometry) -> Vec<u8> {
    let mut pixels = Vec::with_capacity(screen.width as usize * screen.height as usize * 3);
    for y in 0..screen.height {
        for x in 0..screen.width {
            let grid = u8::from(x % 64 < 2 || y % 32 < 2) * 20;
            pixels.extend_from_slice(&[
                12_u8.saturating_add(grid),
                16_u8.saturating_add(grid),
                20_u8.saturating_add(grid),
            ]);
        }
    }
    pixels
}

fn damage_frame(width: u32, height: u32, phase: u8) -> Vec<u8> {
    let mut pixels = Vec::with_capacity(width as usize * height as usize * 3);
    for y in 0..height {
        for x in 0..width {
            let value = ((x.wrapping_mul(13) ^ y.wrapping_mul(7) ^ u32::from(phase)) & 0xff) as u8;
            pixels.extend_from_slice(&[value, value.wrapping_add(47), value.wrapping_add(113)]);
        }
    }
    pixels
}

fn send_barrier(writer: &mut impl Write, image_id: u32) -> io::Result<()> {
    write!(
        writer,
        "\x1b_Gi={image_id},s=1,v=1,a=q,t=d,f=24,q=0;AAAA\x1b\\"
    )
}

fn wait_for_response(image_id: u32) -> io::Result<()> {
    let mut response = Vec::new();
    let mut byte = [0_u8; 1];
    let mut in_response = false;
    let mut previous_escape = false;
    loop {
        let mut pollfd = libc::pollfd {
            fd: io::stdin().as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        let ready = unsafe { libc::poll(&mut pollfd, 1, RESPONSE_TIMEOUT_MS) };
        if ready == 0 {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                format!(
                    "timed out waiting for Kitty graphics acknowledgement; partial input={response:?}"
                ),
            ));
        }
        if ready < 0 {
            return Err(io::Error::last_os_error());
        }
        let count = unsafe { libc::read(io::stdin().as_raw_fd(), byte.as_mut_ptr().cast(), 1) };
        if count != 1 {
            return Err(if count < 0 {
                io::Error::last_os_error()
            } else {
                io::Error::new(io::ErrorKind::UnexpectedEof, "terminal input closed")
            });
        }
        if !in_response {
            response.push(byte[0]);
            if response.ends_with(b"\x1b_G") {
                response.clear();
                response.extend_from_slice(b"\x1b_G");
                in_response = true;
            } else if response.len() > 3 {
                response.remove(0);
            }
            continue;
        }
        response.push(byte[0]);
        if previous_escape && byte[0] == b'\\' {
            let text = String::from_utf8_lossy(&response);
            if !text.contains(&format!("i={image_id}")) {
                response.clear();
                in_response = false;
                previous_escape = false;
                continue;
            }
            if !text.contains(";OK") {
                return Err(io::Error::other(format!(
                    "Kitty graphics command failed: {text:?}"
                )));
            }
            return Ok(());
        }
        previous_escape = byte[0] == 0x1b;
    }
}
