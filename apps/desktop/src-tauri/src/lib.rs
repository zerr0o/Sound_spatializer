#[cfg(all(not(debug_assertions), not(feature = "custom-protocol")))]
compile_error!(
    "Un build Release Tauri doit activer `custom-protocol`. Utilisez `pnpm build:ui` ou `pnpm --dir apps/desktop package`, pas `cargo build --release`."
);

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicBool, AtomicU64, Ordering},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tauri::{
    image::Image,
    ipc::{InvokeBody, Request},
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Manager, RunEvent, State, WindowEvent,
};

const MAX_JSON_BYTES: usize = 1 << 20;
const MAX_SOFA_BYTES: u64 = 512 * 1024 * 1024;
const MAX_EQ_BYTES: u64 = 1 << 20;
const MAX_EQ_FILTERS: usize = 16;
const ENGINE_COMMAND_TIMEOUT: Duration = Duration::from_secs(15);
const POSE_RECONNECT_MIN_DELAY: Duration = Duration::from_millis(2);
const POSE_RECONNECT_MAX_DELAY: Duration = Duration::from_millis(20);
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

struct PipeShared {
    endpoint: Result<String, String>,
    writer: Mutex<Option<Arc<Mutex<File>>>>,
    latest_status: Mutex<Option<String>>,
    next_command_id: AtomicU64,
    next_connection_generation: AtomicU64,
    active_connection_generation: AtomicU64,
    pending_commands: Mutex<HashMap<u32, Arc<PendingEngineCommand>>>,
    connection: Mutex<()>,
}

struct PendingEngineCommand {
    connection_generation: u64,
    outcome: Mutex<Option<Result<(), String>>>,
    ready: Condvar,
}

impl PendingEngineCommand {
    fn new(connection_generation: u64) -> Self {
        Self {
            connection_generation,
            outcome: Mutex::new(None),
            ready: Condvar::new(),
        }
    }
}

struct PosePipeShared {
    endpoint: Result<String, String>,
    writer: Mutex<Option<File>>,
}

impl Default for PosePipeShared {
    fn default() -> Self {
        Self {
            endpoint: current_pose_pipe_name(),
            writer: Mutex::new(None),
        }
    }
}

impl Default for PipeShared {
    fn default() -> Self {
        Self {
            endpoint: current_engine_pipe_name(),
            writer: Mutex::new(None),
            latest_status: Mutex::new(None),
            next_command_id: AtomicU64::new(1),
            next_connection_generation: AtomicU64::new(1),
            active_connection_generation: AtomicU64::new(0),
            pending_commands: Mutex::new(HashMap::new()),
            connection: Mutex::new(()),
        }
    }
}

fn validate_pipe_user_sid(user_sid: &str) -> Result<(), String> {
    if !user_sid.starts_with("S-")
        || user_sid.len() > 184
        || !user_sid
            .bytes()
            .all(|byte| byte.is_ascii_digit() || byte == b'S' || byte == b'-')
    {
        return Err("SID utilisateur invalide pour le nom du pipe".into());
    }
    Ok(())
}

fn format_engine_pipe_name(user_sid: &str, session_id: u32) -> Result<String, String> {
    validate_pipe_user_sid(user_sid)?;
    Ok(format!(
        r"\\.\pipe\SoundSpatializer.Engine.v1.{user_sid}.{session_id}"
    ))
}

fn format_pose_pipe_name(user_sid: &str, session_id: u32) -> Result<String, String> {
    validate_pipe_user_sid(user_sid)?;
    Ok(format!(
        r"\\.\pipe\SoundSpatializer.Pose.v1.{user_sid}.{session_id}"
    ))
}

#[cfg(windows)]
fn current_pipe_identity() -> Result<(String, u32), String> {
    use windows::{
        core::PWSTR,
        Win32::{
            Foundation::{CloseHandle, LocalFree, HANDLE, HLOCAL},
            Security::{
                Authorization::ConvertSidToStringSidW, GetTokenInformation, TokenUser, TOKEN_QUERY,
                TOKEN_USER,
            },
            System::{
                RemoteDesktop::ProcessIdToSessionId,
                Threading::{GetCurrentProcess, GetCurrentProcessId, OpenProcessToken},
            },
        },
    };

    struct TokenHandle(HANDLE);
    impl Drop for TokenHandle {
        fn drop(&mut self) {
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }

    unsafe {
        let mut raw_token = HANDLE::default();
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut raw_token)
            .map_err(|error| error.to_string())?;
        let token = TokenHandle(raw_token);
        let mut required = 0_u32;
        let _ = GetTokenInformation(token.0, TokenUser, None, 0, &mut required);
        if required < std::mem::size_of::<TOKEN_USER>() as u32 {
            return Err("Informations de jeton utilisateur indisponibles".into());
        }
        let mut buffer = vec![0_u8; required as usize];
        GetTokenInformation(
            token.0,
            TokenUser,
            Some(buffer.as_mut_ptr().cast()),
            required,
            &mut required,
        )
        .map_err(|error| error.to_string())?;
        let token_user = &*(buffer.as_ptr().cast::<TOKEN_USER>());
        let mut sid_pointer = PWSTR::null();
        ConvertSidToStringSidW(token_user.User.Sid, &mut sid_pointer)
            .map_err(|error| error.to_string())?;
        let sid = sid_pointer.to_string().map_err(|error| error.to_string());
        let _ = LocalFree(Some(HLOCAL(sid_pointer.as_ptr().cast())));
        let sid = sid?;
        let mut session_id = 0_u32;
        ProcessIdToSessionId(GetCurrentProcessId(), &mut session_id)
            .map_err(|error| error.to_string())?;
        Ok((sid, session_id))
    }
}

#[cfg(not(windows))]
fn current_pipe_identity() -> Result<(String, u32), String> {
    Ok(("S-1-0-0".into(), 0))
}

fn current_engine_pipe_name() -> Result<String, String> {
    let (sid, session_id) = current_pipe_identity()?;
    format_engine_pipe_name(&sid, session_id)
}

fn current_pose_pipe_name() -> Result<String, String> {
    let (sid, session_id) = current_pipe_identity()?;
    format_pose_pipe_name(&sid, session_id)
}

#[derive(Default)]
struct PoseMailboxState {
    latest: Option<[u8; 64]>,
    highest_sequence: u64,
}

#[derive(Default)]
struct PoseMailbox {
    state: Mutex<PoseMailboxState>,
    ready: Condvar,
    received: AtomicU64,
    accepted: AtomicU64,
    superseded: AtomicU64,
    taken: AtomicU64,
    written: AtomicU64,
    write_errors: AtomicU64,
}

impl PoseMailbox {
    fn publish(&self, packet: [u8; 64]) -> Result<bool, String> {
        self.received.fetch_add(1, Ordering::Relaxed);
        let sequence = u64::from_le_bytes(
            packet[8..16]
                .try_into()
                .map_err(|_| "Séquence de pose invalide".to_string())?,
        );
        let mut state = self
            .state
            .lock()
            .map_err(|_| "Mailbox pose empoisonnée".to_string())?;
        if sequence <= state.highest_sequence {
            return Ok(false);
        }
        if state.latest.is_some() {
            self.superseded.fetch_add(1, Ordering::Relaxed);
        }
        state.highest_sequence = sequence;
        state.latest = Some(packet);
        self.accepted.fetch_add(1, Ordering::Relaxed);
        drop(state);
        self.ready.notify_one();
        Ok(true)
    }

    fn wait_and_take_latest(&self) -> Result<[u8; 64], ()> {
        let mut state = self.state.lock().map_err(|_| ())?;
        while state.latest.is_none() {
            state = self.ready.wait(state).map_err(|_| ())?;
        }
        let packet = state.latest.take().ok_or(())?;
        self.taken.fetch_add(1, Ordering::Relaxed);
        Ok(packet)
    }

    fn record_write(&self, succeeded: bool) {
        if succeeded {
            self.written.fetch_add(1, Ordering::Relaxed);
        } else {
            self.write_errors.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn diagnostics(&self) -> serde_json::Value {
        serde_json::json!({
            "received": self.received.load(Ordering::Relaxed),
            "accepted": self.accepted.load(Ordering::Relaxed),
            "superseded": self.superseded.load(Ordering::Relaxed),
            "taken": self.taken.load(Ordering::Relaxed),
            "written": self.written.load(Ordering::Relaxed),
            "writeErrors": self.write_errors.load(Ordering::Relaxed)
        })
    }
}

struct DesktopState {
    process: Mutex<Option<Child>>,
    pipe: Arc<PipeShared>,
    _pose_pipe: Arc<PosePipeShared>,
    validated_hrtf: Mutex<HashMap<String, PathBuf>>,
    pose_mailbox: Arc<PoseMailbox>,
    supervisor_started: AtomicBool,
}

impl Default for DesktopState {
    fn default() -> Self {
        let pipe = Arc::new(PipeShared::default());
        let pose_pipe = Arc::new(PosePipeShared::default());
        let pose_mailbox = Arc::new(PoseMailbox::default());
        let pose_pipe_for_thread = Arc::clone(&pose_pipe);
        let mailbox_for_thread = Arc::clone(&pose_mailbox);
        thread::Builder::new()
            .name("ssp-pose-writer".into())
            .spawn(move || {
                let mut reconnect_delay = POSE_RECONNECT_MIN_DELAY;
                loop {
                    if connect_pose_pipe(&pose_pipe_for_thread).is_err() {
                        thread::sleep(reconnect_delay);
                        reconnect_delay = std::cmp::min(
                            reconnect_delay.saturating_mul(2),
                            POSE_RECONNECT_MAX_DELAY,
                        );
                        continue;
                    }
                    reconnect_delay = POSE_RECONNECT_MIN_DELAY;

                    // La connexion est prête avant de retirer la pose : même si
                    // l'ouverture du serveur a tardé, seul l'échantillon le plus
                    // récent quitte maintenant la mailbox.
                    let packet = match mailbox_for_thread.wait_and_take_latest() {
                        Ok(packet) => packet,
                        Err(()) => return,
                    };
                    let succeeded =
                        write_connected_pose_pipe(&pose_pipe_for_thread, &packet).is_ok();
                    mailbox_for_thread.record_write(succeeded);
                    // Une écriture cassée invalide le writer. L'itération suivante
                    // doit reconnecter avant de prélever une autre pose.
                }
            })
            .expect("impossible de démarrer la thread de pose");
        Self {
            process: Mutex::new(None),
            pipe,
            _pose_pipe: pose_pipe,
            validated_hrtf: Mutex::new(HashMap::new()),
            pose_mailbox,
            supervisor_started: AtomicBool::new(false),
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ImportedSofa {
    file_name: String,
    hash: String,
    local_path: String,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct ImportedHeadphoneEq {
    file_name: String,
    format: String,
    profile_name: String,
    preamp_db: f64,
    bands: Vec<ImportedHeadphoneEqBand>,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct ImportedHeadphoneEqBand {
    id: String,
    enabled: bool,
    #[serde(rename = "type")]
    filter_type: String,
    frequency_hz: f64,
    gain_db: f64,
    q: f64,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct HeadphoneEqJsonV1 {
    format: String,
    schema_version: u32,
    profile_name: String,
    preamp_db: f64,
    filters: Vec<HeadphoneEqJsonFilterV1>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct HeadphoneEqJsonFilterV1 {
    enabled: bool,
    #[serde(rename = "type")]
    filter_type: HeadphoneEqJsonFilterType,
    frequency_hz: f64,
    gain_db: f64,
    q: f64,
}

#[derive(Clone, Copy, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum HeadphoneEqJsonFilterType {
    Peak,
    LowShelf,
    HighShelf,
}

impl HeadphoneEqJsonFilterType {
    fn as_internal_name(self) -> &'static str {
        match self {
            Self::Peak => "peak",
            Self::LowShelf => "low-shelf",
            Self::HighShelf => "high-shelf",
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct QpcSnapshot {
    ticks: String,
    frequency: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AudioDeviceSummary {
    id: String,
    name: String,
    is_default: bool,
    is_sound_spatializer_endpoint: bool,
    transport: &'static str,
    sample_rate: u32,
    channel_count: u16,
    channel_mask: u32,
}

fn app_data_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let path = app
        .path()
        .local_data_dir()
        .map_err(|error| error.to_string())?
        .join("SoundSpatializer");
    fs::create_dir_all(&path)
        .map_err(|error| format!("Création du dossier de configuration impossible : {error}"))?;
    Ok(path)
}

fn atomic_write(path: &Path, contents: &[u8]) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| "Chemin de destination invalide".to_string())?;
    fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    let temporary = temporary_path(
        parent,
        path.file_name()
            .and_then(|value| value.to_str())
            .unwrap_or("config"),
    );
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&temporary)
            .map_err(|error| error.to_string())?;
        file.write_all(contents)
            .map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())?;
        atomic_replace(&temporary, path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn temporary_path(parent: &Path, label: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(".{label}.{}.{}.tmp", std::process::id(), sequence))
}

#[cfg(windows)]
fn atomic_replace(source: &Path, destination: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::PCWSTR;
    use windows::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    let source: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let destination: Vec<u16> = destination
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect();
    unsafe {
        MoveFileExW(
            PCWSTR(source.as_ptr()),
            PCWSTR(destination.as_ptr()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    }
    .map_err(|error| error.to_string())
}

#[cfg(not(windows))]
fn atomic_replace(source: &Path, destination: &Path) -> Result<(), String> {
    fs::rename(source, destination).map_err(|error| error.to_string())
}

#[tauri::command(async)]
fn load_app_config(app: AppHandle) -> Result<Option<String>, String> {
    let directory = app_data_dir(&app)?;
    for file_name in [
        "desktop-config-v3.json",
        "desktop-config-v2.json",
        "desktop-config-v1.json",
    ] {
        let path = directory.join(file_name);
        match fs::metadata(&path) {
            Ok(metadata) if !metadata.is_file() || metadata.len() > MAX_JSON_BYTES as u64 => {
                return Err("Configuration locale invalide ou trop volumineuse".into());
            }
            Ok(_) => {
                let value = fs::read_to_string(path).map_err(|error| error.to_string())?;
                validate_app_config_json(&value)?;
                return Ok(Some(value));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(None)
}

fn validate_app_config_json(payload: &str) -> Result<(), String> {
    if payload.is_empty() || payload.len() > MAX_JSON_BYTES {
        return Err("Configuration trop volumineuse".into());
    }
    let document: serde_json::Value =
        serde_json::from_str(payload).map_err(|error| format!("JSON invalide : {error}"))?;
    let envelope_version = document
        .get("schemaVersion")
        .and_then(serde_json::Value::as_u64)
        .unwrap_or_default();
    let scene_version = document
        .pointer("/scene/schemaVersion")
        .and_then(serde_json::Value::as_u64)
        .unwrap_or_default();
    if !matches!((envelope_version, scene_version), (1, 1) | (2, 2) | (3, 2))
        || !document
            .get("preferences")
            .is_some_and(serde_json::Value::is_object)
        || (envelope_version == 3
            && !document
                .get("windowSpatialization")
                .is_some_and(serde_json::Value::is_object))
    {
        return Err("Version ou structure de configuration non prise en charge".into());
    }
    Ok(())
}

#[tauri::command(async)]
fn save_app_config(app: AppHandle, payload: String) -> Result<(), String> {
    validate_app_config_json(&payload)?;
    let document: serde_json::Value =
        serde_json::from_str(&payload).map_err(|error| format!("JSON invalide : {error}"))?;
    if document
        .get("schemaVersion")
        .and_then(serde_json::Value::as_u64)
        != Some(3)
    {
        return Err("Seule la configuration courante V3 peut être enregistrée".into());
    }
    atomic_write(
        &app_data_dir(&app)?.join("desktop-config-v3.json"),
        payload.as_bytes(),
    )
}

#[tauri::command(async)]
fn import_sofa(app: AppHandle, source: String) -> Result<ImportedSofa, String> {
    let source = PathBuf::from(source);
    if source
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| value.eq_ignore_ascii_case("sofa"))
        != Some(true)
    {
        return Err("Le fichier doit utiliser l’extension .sofa".into());
    }
    let metadata = fs::metadata(&source)
        .map_err(|error| format!("Lecture du fichier impossible : {error}"))?;
    if !metadata.is_file() || metadata.len() < 512 || metadata.len() > MAX_SOFA_BYTES {
        return Err("Taille de fichier SOFA invalide".into());
    }
    let directory = app_data_dir(&app)?.join("hrtf").join("imported");
    fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let temporary = temporary_path(&directory, "sofa-import");
    let mut input = File::open(&source).map_err(|error| error.to_string())?;
    let import_result: Result<(String, PathBuf), String> = (|| {
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|error| error.to_string())?;
        let mut hasher = Sha256::new();
        let mut header = [0_u8; 8];
        input
            .read_exact(&mut header)
            .map_err(|error| error.to_string())?;
        if header != [0x89, b'H', b'D', b'F', 0x0d, 0x0a, 0x1a, 0x0a] && &header[..3] != b"CDF" {
            return Err("Le contenu ne ressemble pas à un conteneur SOFA/HDF5".into());
        }
        output
            .write_all(&header)
            .map_err(|error| error.to_string())?;
        hasher.update(header);
        let mut total = header.len() as u64;
        let mut buffer = [0_u8; 64 * 1024];
        loop {
            let read = input.read(&mut buffer).map_err(|error| error.to_string())?;
            if read == 0 {
                break;
            }
            total = total
                .checked_add(read as u64)
                .ok_or_else(|| "Fichier SOFA trop volumineux".to_string())?;
            if total > MAX_SOFA_BYTES || total > metadata.len() {
                return Err("Le fichier SOFA a changé pendant l’import".into());
            }
            hasher.update(&buffer[..read]);
            output
                .write_all(&buffer[..read])
                .map_err(|error| error.to_string())?;
        }
        if total != metadata.len() {
            return Err("Le fichier SOFA a changé pendant l’import".into());
        }
        output.sync_all().map_err(|error| error.to_string())?;
        let hash = hex::encode(hasher.finalize());
        let destination = directory.join(format!("{hash}.sofa"));
        if destination.exists() {
            validate_hashed_sofa(&directory, &destination)?;
            fs::remove_file(&temporary).map_err(|error| error.to_string())?;
        } else {
            atomic_replace(&temporary, &destination)?;
        }
        Ok((hash, destination))
    })();
    if import_result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    let (hash, destination) = import_result?;
    Ok(ImportedSofa {
        file_name: source
            .file_name()
            .and_then(|value| value.to_str())
            .unwrap_or("profile.sofa")
            .to_string(),
        hash,
        local_path: destination.to_string_lossy().into_owned(),
    })
}

fn finite_clamped(value: f64, label: &str, minimum: f64, maximum: f64) -> Result<f64, String> {
    if !value.is_finite() {
        return Err(format!("{label} doit être un nombre fini"));
    }
    Ok(value.clamp(minimum, maximum))
}

fn validate_eq_profile_name(value: &str) -> Result<String, String> {
    let value = value.trim();
    if value.is_empty() || value.chars().count() > 128 || value.chars().any(char::is_control) {
        return Err(
            "Le nom du profil EQ doit contenir entre 1 et 128 caractères imprimables".into(),
        );
    }
    Ok(value.to_string())
}

fn imported_eq_band(
    id: String,
    enabled: bool,
    filter_type: &str,
    frequency_hz: f64,
    gain_db: f64,
    q: f64,
) -> Result<ImportedHeadphoneEqBand, String> {
    Ok(ImportedHeadphoneEqBand {
        id,
        enabled,
        filter_type: filter_type.to_string(),
        frequency_hz: finite_clamped(frequency_hz, "La fréquence", 10.0, 24_000.0)?,
        gain_db: finite_clamped(gain_db, "Le gain", -24.0, 24.0)?,
        q: finite_clamped(q, "Le facteur Q", 0.01, 30.0)?,
    })
}

fn parse_headphone_eq_json(contents: &str, file_name: &str) -> Result<ImportedHeadphoneEq, String> {
    let document: HeadphoneEqJsonV1 =
        serde_json::from_str(contents).map_err(|error| format!("JSON EQ V1 invalide : {error}"))?;
    if document.format != "sound-spatializer-headphone-eq" || document.schema_version != 1 {
        return Err("Format ou version JSON EQ non pris en charge".into());
    }
    if document.filters.is_empty() || document.filters.len() > MAX_EQ_FILTERS {
        return Err(format!(
            "Un profil EQ doit contenir entre 1 et {MAX_EQ_FILTERS} filtres"
        ));
    }
    let profile_name = validate_eq_profile_name(&document.profile_name)?;
    let preamp_db = finite_clamped(document.preamp_db, "Le préampli", -24.0, 0.0)?;
    let bands = document
        .filters
        .into_iter()
        .enumerate()
        .map(|(index, filter)| {
            imported_eq_band(
                format!("import-json-{}", index + 1),
                filter.enabled,
                filter.filter_type.as_internal_name(),
                filter.frequency_hz,
                filter.gain_db,
                filter.q,
            )
        })
        .collect::<Result<Vec<_>, _>>()?;
    if !bands.iter().any(|band| band.enabled) {
        return Err("Le profil EQ ne contient aucun filtre actif".into());
    }
    Ok(ImportedHeadphoneEq {
        file_name: file_name.to_string(),
        format: "sound-spatializer-json-v1".into(),
        profile_name,
        preamp_db,
        bands,
    })
}

fn parse_eq_number(value: &str, line_number: usize, label: &str) -> Result<f64, String> {
    let number = value
        .parse::<f64>()
        .map_err(|_| format!("Ligne {line_number} : {label} doit être un nombre décimal"))?;
    if !number.is_finite() {
        return Err(format!("Ligne {line_number} : {label} doit être fini"));
    }
    Ok(number)
}

fn token_is(value: &str, expected: &str) -> bool {
    value.eq_ignore_ascii_case(expected)
}

fn parse_equalizer_apo(
    contents: &str,
    file_name: &str,
    fallback_profile_name: &str,
) -> Result<ImportedHeadphoneEq, String> {
    let mut preamp_db = None;
    let mut bands = Vec::new();
    let mut filter_numbers = HashSet::new();

    for (line_index, source_line) in contents.lines().enumerate() {
        let line_number = line_index + 1;
        let line = source_line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        let tokens = line.split_whitespace().collect::<Vec<_>>();
        if tokens
            .first()
            .is_some_and(|token| token_is(token, "Preamp:"))
        {
            if tokens.len() != 3 || !token_is(tokens[2], "dB") {
                return Err(format!(
                    "Ligne {line_number} : syntaxe attendue « Preamp: -6.0 dB »"
                ));
            }
            if preamp_db.is_some() {
                return Err(format!(
                    "Ligne {line_number} : préampli défini plusieurs fois"
                ));
            }
            preamp_db = Some(parse_eq_number(tokens[1], line_number, "le préampli")?);
            continue;
        }
        if !tokens
            .first()
            .is_some_and(|token| token_is(token, "Filter"))
        {
            return Err(format!(
                "Ligne {line_number} : directive Equalizer APO non prise en charge"
            ));
        }
        if tokens.len() != 12
            || !tokens[1].ends_with(':')
            || !(token_is(tokens[2], "ON") || token_is(tokens[2], "OFF"))
            || !token_is(tokens[4], "Fc")
            || !token_is(tokens[6], "Hz")
            || !token_is(tokens[7], "Gain")
            || !token_is(tokens[9], "dB")
            || !token_is(tokens[10], "Q")
        {
            return Err(format!(
                "Ligne {line_number} : syntaxe attendue « Filter n: ON PK/LS/HS Fc … Hz Gain … dB Q … »"
            ));
        }
        if bands.len() >= MAX_EQ_FILTERS {
            return Err(format!(
                "Le profil dépasse la limite de {MAX_EQ_FILTERS} filtres"
            ));
        }
        let filter_number = tokens[1][..tokens[1].len() - 1]
            .parse::<u32>()
            .map_err(|_| format!("Ligne {line_number} : numéro de filtre invalide"))?;
        if filter_number == 0 || !filter_numbers.insert(filter_number) {
            return Err(format!(
                "Ligne {line_number} : numéro de filtre nul ou dupliqué"
            ));
        }
        let filter_type = if token_is(tokens[3], "PK") {
            "peak"
        } else if token_is(tokens[3], "LS") || token_is(tokens[3], "LSC") {
            "low-shelf"
        } else if token_is(tokens[3], "HS") || token_is(tokens[3], "HSC") {
            "high-shelf"
        } else {
            return Err(format!(
                "Ligne {line_number} : seuls les filtres PK, LS/LSC et HS/HSC sont acceptés"
            ));
        };
        bands.push(imported_eq_band(
            format!("import-apo-{filter_number}"),
            token_is(tokens[2], "ON"),
            filter_type,
            parse_eq_number(tokens[5], line_number, "la fréquence")?,
            parse_eq_number(tokens[8], line_number, "le gain")?,
            parse_eq_number(tokens[11], line_number, "le facteur Q")?,
        )?);
    }

    if bands.is_empty() {
        return Err("Le fichier Equalizer APO ne contient aucun filtre".into());
    }
    if !bands.iter().any(|band| band.enabled) {
        return Err("Le profil EQ ne contient aucun filtre actif".into());
    }
    Ok(ImportedHeadphoneEq {
        file_name: file_name.to_string(),
        format: "equalizer-apo".into(),
        profile_name: validate_eq_profile_name(fallback_profile_name)?,
        preamp_db: finite_clamped(
            preamp_db.ok_or_else(|| "La directive Preamp est obligatoire".to_string())?,
            "Le préampli",
            -24.0,
            0.0,
        )?,
        bands,
    })
}

#[cfg(windows)]
fn ensure_local_windows_path(path: &Path) -> Result<(), String> {
    use std::path::{Component, Prefix};
    match path.components().next() {
        Some(Component::Prefix(prefix)) => match prefix.kind() {
            Prefix::Disk(_) | Prefix::VerbatimDisk(_) => Ok(()),
            _ => Err("Les chemins réseau, UNC et périphérique ne sont pas acceptés".into()),
        },
        _ => Err("Le fichier EQ doit utiliser un chemin local absolu".into()),
    }
}

#[cfg(not(windows))]
fn ensure_local_windows_path(path: &Path) -> Result<(), String> {
    if path.is_absolute() {
        Ok(())
    } else {
        Err("Le fichier EQ doit utiliser un chemin local absolu".into())
    }
}

fn read_headphone_eq(source: &Path) -> Result<ImportedHeadphoneEq, String> {
    if !source.is_absolute() {
        return Err("Le fichier EQ doit utiliser un chemin local absolu".into());
    }
    ensure_local_windows_path(source)?;
    let source = fs::canonicalize(source)
        .map_err(|error| format!("Fichier EQ local inaccessible : {error}"))?;
    ensure_local_windows_path(&source)?;
    let extension = source
        .extension()
        .and_then(|value| value.to_str())
        .map(str::to_ascii_lowercase)
        .ok_or_else(|| "Extension de fichier EQ absente".to_string())?;
    if extension != "json" && extension != "txt" {
        return Err("Le fichier EQ doit utiliser l’extension .json ou .txt".into());
    }
    let metadata = fs::metadata(&source)
        .map_err(|error| format!("Lecture du fichier EQ impossible : {error}"))?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_EQ_BYTES {
        return Err("Le fichier EQ doit être non vide et ne pas dépasser 1 Mio".into());
    }
    let mut bytes = Vec::with_capacity((metadata.len() + 1) as usize);
    File::open(&source)
        .map_err(|error| error.to_string())?
        .take(MAX_EQ_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| error.to_string())?;
    if bytes.len() as u64 > MAX_EQ_BYTES || bytes.len() as u64 != metadata.len() {
        return Err("Le fichier EQ a changé pendant l’import ou dépasse 1 Mio".into());
    }
    let contents = String::from_utf8(bytes)
        .map_err(|_| "Le fichier EQ doit être encodé en UTF-8".to_string())?;
    if contents.contains('\0') {
        return Err("Le fichier EQ contient un caractère nul interdit".into());
    }
    let contents = contents.strip_prefix('\u{feff}').unwrap_or(&contents);
    let file_name = source
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "Nom de fichier EQ invalide".to_string())?;
    if extension == "json" {
        parse_headphone_eq_json(contents, file_name)
    } else {
        let profile_name = source
            .file_stem()
            .and_then(|value| value.to_str())
            .ok_or_else(|| "Nom de profil EQ invalide".to_string())?;
        parse_equalizer_apo(contents, file_name, profile_name)
    }
}

#[tauri::command(async)]
fn import_headphone_eq(source: String) -> Result<ImportedHeadphoneEq, String> {
    read_headphone_eq(Path::new(&source))
}

fn executable_engine_candidates(executable: &Path) -> Vec<PathBuf> {
    let Some(binary_directory) = executable.parent() else {
        return Vec::new();
    };
    let sibling = binary_directory.join("SoundSpatializer.Engine.exe");
    let mut result = Vec::new();

    // A repository Release/Debug executable lives under
    // apps/desktop/src-tauri/target/<profile>. Derive the workspace root from
    // that stable shape instead of embedding the build machine's absolute path.
    // Installed builds intentionally do not match this shape and use the sibling
    // engine placed by WiX above.
    let profile = binary_directory
        .file_name()
        .and_then(|value| value.to_str());
    let target = binary_directory.parent();
    let src_tauri = target.and_then(Path::parent);
    let desktop = src_tauri.and_then(Path::parent);
    let apps = desktop.and_then(Path::parent);
    let workspace = apps.and_then(Path::parent);
    let repository_layout = matches!(profile, Some("release" | "debug"))
        && target
            .and_then(Path::file_name)
            .and_then(|value| value.to_str())
            == Some("target")
        && src_tauri
            .and_then(Path::file_name)
            .and_then(|value| value.to_str())
            == Some("src-tauri")
        && desktop
            .and_then(Path::file_name)
            .and_then(|value| value.to_str())
            == Some("desktop")
        && apps
            .and_then(Path::file_name)
            .and_then(|value| value.to_str())
            == Some("apps");
    if repository_layout {
        if let Some(workspace) = workspace {
            // Development must not silently reuse a stale executable copied to
            // target/debug by an earlier run. Prefer the reproducible workspace
            // build; release and installed binaries remain self-contained.
            if profile == Some("debug") {
                result.push(
                    workspace
                        .join("build/engine-mysofa/Debug")
                        .join("SoundSpatializer.Engine.exe"),
                );
                result.push(
                    workspace
                        .join("build/engine-dev/Debug")
                        .join("SoundSpatializer.Engine.exe"),
                );
            }
            result.push(sibling.clone());
            result.push(
                workspace
                    .join("build/engine-mysofa/Release")
                    .join("SoundSpatializer.Engine.exe"),
            );
            result.push(
                workspace
                    .join("build/engine/Release")
                    .join("SoundSpatializer.Engine.exe"),
            );
            result.push(
                workspace
                    .join("build/engine/Debug")
                    .join("SoundSpatializer.Engine.exe"),
            );
        }
    } else {
        result.push(sibling.clone());
    }
    if result.is_empty() {
        result.push(sibling);
    }
    result
}

fn engine_candidates(app: &AppHandle) -> Vec<PathBuf> {
    let mut result = Vec::new();
    if let Some(explicit) = std::env::var_os("SOUND_SPATIALIZER_ENGINE") {
        result.push(PathBuf::from(explicit));
    }
    if let Ok(executable) = std::env::current_exe() {
        result.extend(executable_engine_candidates(&executable));
    }
    if let Ok(resources) = app.path().resource_dir() {
        result.push(resources.join("engine").join("SoundSpatializer.Engine.exe"));
        result.push(resources.join("SoundSpatializer.Engine.exe"));
    }
    result
}

#[tauri::command(async)]
fn start_engine(app: AppHandle, state: State<'_, DesktopState>) -> Result<(), String> {
    if !state.supervisor_started.swap(true, Ordering::AcqRel) {
        let supervisor_app = app.clone();
        if let Err(error) = thread::Builder::new()
            .name("ssp-engine-supervisor".into())
            .spawn(move || loop {
                thread::sleep(Duration::from_secs(2));
                let state = supervisor_app.state::<DesktopState>();
                let pipe_connected = state
                    .pipe
                    .writer
                    .lock()
                    .map(|writer| writer.is_some())
                    .unwrap_or(false);
                if !pipe_connected
                    && connect_pipe_with_retry(&state.pipe, 1, Duration::ZERO).is_ok()
                {
                    continue;
                }
                let needs_restart = !pipe_connected
                    && state
                        .process
                        .lock()
                        .map(|mut process| match process.as_mut() {
                            Some(child) => child
                                .try_wait()
                                .map(|status| status.is_some())
                                .unwrap_or(true),
                            None => true,
                        })
                        .unwrap_or(false);
                if needs_restart {
                    let _ = ensure_engine_process(&supervisor_app, &state);
                }
            })
        {
            state.supervisor_started.store(false, Ordering::Release);
            return Err(error.to_string());
        }
    }
    ensure_engine_process(&app, &state)?;
    // Le moteur publie son statut toutes les 250 ms. Attendre le premier
    // snapshot permet de négocier l'ACK de commandes avant le bootstrap, tout
    // en restant compatible avec un ancien moteur V1 déjà lancé.
    for _ in 0..30 {
        if state
            .pipe
            .latest_status
            .lock()
            .map(|status| status.is_some())
            .unwrap_or(false)
        {
            break;
        }
        thread::sleep(Duration::from_millis(25));
    }
    Ok(())
}

const ENGINE_SHUTDOWN_POLL: Duration = Duration::from_millis(25);
const ENGINE_SHUTDOWN_TIMEOUT: Duration = Duration::from_millis(2_500);

/// Arrête le moteur audio quand l'interface se termine.
///
/// Le moteur est un processus distinct qui peut survivre à l'UI : il est parfois
/// lancé à l'ouverture de session, et `ensure_engine_process` se raccroche
/// volontairement à une instance déjà présente. Tuer un enfant que l'on ne
/// possède pas toujours ne suffit donc pas ; la demande passe d'abord par le
/// pipe, ce qui laisse le moteur libérer WASAPI et écrire sa configuration.
/// La terminaison forcée ne sert que de garde-fou.
fn shutdown_engine(state: &DesktopState) {
    let payload = "{\"schemaVersion\":1,\"type\":\"shutdown\"}";
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    frame.extend_from_slice(payload.as_bytes());
    // Un moteur déjà absent fait échouer l'écriture : c'est le résultat voulu,
    // il n'y a alors rien à arrêter.
    let requested = write_pipe(&state.pipe, &frame).is_ok();

    let Ok(mut process) = state.process.lock() else {
        return;
    };
    let Some(child) = process.as_mut() else {
        // Le moteur appartient à une autre instance ou à la session : la
        // commande ci-dessus est le seul levier disponible, et elle suffit.
        return;
    };

    if requested {
        let deadline = std::time::Instant::now() + ENGINE_SHUTDOWN_TIMEOUT;
        while std::time::Instant::now() < deadline {
            match child.try_wait() {
                Ok(Some(_)) => {
                    *process = None;
                    return;
                }
                Ok(None) => thread::sleep(ENGINE_SHUTDOWN_POLL),
                Err(_) => break,
            }
        }
    }

    // Sortie propre refusée ou impossible : ne pas laisser un moteur orphelin
    // tenir le périphérique audio sans interface pour le piloter.
    let _ = child.kill();
    let _ = child.wait();
    *process = None;
}

fn ensure_engine_process(app: &AppHandle, state: &DesktopState) -> Result<(), String> {
    // Le moteur peut avoir été lancé à l'ouverture de session ou survivre à l'UI.
    // Une connexion réussie prouve sa présence et évite de créer un second processus.
    if connect_pipe_with_retry(&state.pipe, 1, Duration::ZERO).is_ok() {
        return Ok(());
    }

    let should_spawn = {
        let mut process = state
            .process
            .lock()
            .map_err(|_| "Verrou moteur empoisonné".to_string())?;
        if let Some(child) = process.as_mut() {
            if child
                .try_wait()
                .map_err(|error| error.to_string())?
                .is_none()
            {
                false
            } else {
                *process = None;
                true
            }
        } else {
            true
        }
    };

    if should_spawn {
        let executable = engine_candidates(app).into_iter().find(|path| path.is_file()).ok_or_else(|| {
            "SoundSpatializer.Engine.exe est introuvable. Compilez le moteur ou définissez SOUND_SPATIALIZER_ENGINE.".to_string()
        })?;
        let mut command = Command::new(&executable);
        command
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        let child = command
            .spawn()
            .map_err(|error| format!("Démarrage du moteur impossible : {error}"))?;
        *state
            .process
            .lock()
            .map_err(|_| "Verrou moteur empoisonné".to_string())? = Some(child);
    }

    // Le chargement initial d'un SOFA peut prendre sensiblement plus de 200 ms.
    connect_pipe_with_retry(&state.pipe, 100, Duration::from_millis(50)).map(|_| ())
}

#[derive(Clone)]
struct EnginePipeConnection {
    writer: Arc<Mutex<File>>,
    generation: u64,
}

fn connect_pipe(shared: &Arc<PipeShared>) -> Result<EnginePipeConnection, String> {
    connect_pipe_with_retry(shared, 8, Duration::from_millis(25))
}

fn connect_pipe_with_retry(
    shared: &Arc<PipeShared>,
    attempts: usize,
    delay: Duration,
) -> Result<EnginePipeConnection, String> {
    let _connection = shared
        .connection
        .lock()
        .map_err(|_| "Verrou de connexion IPC empoisonné".to_string())?;
    let endpoint = shared.endpoint.as_ref().map_err(Clone::clone)?;
    if let Some(writer) = shared
        .writer
        .lock()
        .map_err(|_| "Verrou IPC empoisonné".to_string())?
        .as_ref()
    {
        return Ok(EnginePipeConnection {
            writer: Arc::clone(writer),
            generation: shared.active_connection_generation.load(Ordering::Acquire),
        });
    }
    let mut last_error = None;
    for attempt in 0..attempts.max(1) {
        match OpenOptions::new().read(true).write(true).open(endpoint) {
            Ok(file) => {
                let reader = file.try_clone().map_err(|error| error.to_string())?;
                let writer = Arc::new(Mutex::new(file));
                let generation = loop {
                    let candidate = shared
                        .next_connection_generation
                        .fetch_add(1, Ordering::Relaxed);
                    if candidate != 0 {
                        break candidate;
                    }
                };
                *shared
                    .writer
                    .lock()
                    .map_err(|_| "Verrou IPC empoisonné".to_string())? = Some(Arc::clone(&writer));
                shared
                    .active_connection_generation
                    .store(generation, Ordering::Release);
                let shared_for_thread = Arc::clone(shared);
                let writer_for_thread = Arc::clone(&writer);
                if let Err(error) = thread::Builder::new()
                    .name("ssp-status-reader".into())
                    .spawn(move || {
                        status_reader(reader, &shared_for_thread, generation);
                        let _was_current = shared_for_thread
                            .connection
                            .lock()
                            .ok()
                            .and_then(|_connection| {
                                let mut slot = shared_for_thread.writer.lock().ok()?;
                                if shared_for_thread
                                    .active_connection_generation
                                    .load(Ordering::Acquire)
                                    == generation
                                    && slot.as_ref().is_some_and(|current| {
                                        Arc::ptr_eq(current, &writer_for_thread)
                                    })
                                {
                                    *slot = None;
                                    shared_for_thread
                                        .active_connection_generation
                                        .store(0, Ordering::Release);
                                    // Keep status invalidation in this same
                                    // critical section. Otherwise a freshly
                                    // connected reader could publish its status
                                    // before this stale reader clears the slot.
                                    if let Ok(mut status) = shared_for_thread.latest_status.lock() {
                                        *status = None;
                                    }
                                    Some(true)
                                } else {
                                    Some(false)
                                }
                            })
                            .unwrap_or(false);
                        fail_pending_engine_commands_for_generation(
                            &shared_for_thread,
                            generation,
                            "Connexion au moteur interrompue",
                        );
                    })
                {
                    *shared
                        .writer
                        .lock()
                        .map_err(|_| "Verrou IPC empoisonné".to_string())? = None;
                    shared
                        .active_connection_generation
                        .store(0, Ordering::Release);
                    return Err(error.to_string());
                }
                return Ok(EnginePipeConnection { writer, generation });
            }
            Err(error) => last_error = Some(error),
        }
        if attempt + 1 < attempts && !delay.is_zero() {
            thread::sleep(delay);
        }
    }
    Err(format!(
        "Connexion au moteur impossible : {}",
        last_error
            .map(|error| error.to_string())
            .unwrap_or_default()
    ))
}

fn status_reader(mut reader: File, shared: &Arc<PipeShared>, generation: u64) {
    loop {
        let mut length = [0_u8; 4];
        if reader.read_exact(&mut length).is_err() {
            break;
        }
        let length = u32::from_le_bytes(length) as usize;
        if length == 0 || length > MAX_JSON_BYTES {
            break;
        }
        let mut payload = vec![0_u8; length];
        if reader.read_exact(&mut payload).is_err() {
            break;
        }
        if let Ok(status) = String::from_utf8(payload) {
            dispatch_engine_frame(status, shared, generation);
        }
    }
}

fn dispatch_engine_frame(frame: String, shared: &Arc<PipeShared>, generation: u64) {
    let Ok(value) = serde_json::from_str::<serde_json::Value>(&frame) else {
        return;
    };
    if value.get("kind").and_then(serde_json::Value::as_str) == Some("command-result") {
        let Some(command_id) = value
            .get("commandId")
            .and_then(serde_json::Value::as_u64)
            .and_then(|value| u32::try_from(value).ok())
        else {
            return;
        };
        let accepted = value
            .get("accepted")
            .and_then(serde_json::Value::as_bool)
            .unwrap_or(false);
        // Optional for rolling upgrades from the first ACK-capable engine.
        let persisted = value
            .get("persisted")
            .and_then(serde_json::Value::as_bool)
            .unwrap_or(true);
        let error = value
            .get("error")
            .and_then(serde_json::Value::as_str)
            .unwrap_or("Le moteur a rejeté la commande")
            .to_string();
        let pending = shared
            .pending_commands
            .lock()
            .ok()
            .and_then(|mut commands| {
                let belongs_to_reader = commands
                    .get(&command_id)
                    .is_some_and(|pending| pending.connection_generation == generation);
                belongs_to_reader
                    .then(|| commands.remove(&command_id))
                    .flatten()
            });
        if let Some(pending) = pending {
            if let Ok(mut outcome) = pending.outcome.lock() {
                *outcome = Some(if !accepted {
                    Err(error)
                } else if !persisted {
                    Err(format!(
                        "ENGINE_COMMAND_APPLIED_NOT_PERSISTED commandId={command_id}: {error}"
                    ))
                } else {
                    Ok(())
                });
                pending.ready.notify_all();
            }
        }
        return;
    }
    // Couple publication to reconnect/disconnect. A stale reader can therefore
    // never replace a status already emitted by its successor.
    let Ok(_connection) = shared.connection.lock() else {
        return;
    };
    if shared.active_connection_generation.load(Ordering::Acquire) == generation {
        if let Ok(mut slot) = shared.latest_status.lock() {
            *slot = Some(frame);
        }
    }
}

fn fail_pending_engine_commands_for_generation(shared: &PipeShared, generation: u64, detail: &str) {
    let pending = shared
        .pending_commands
        .lock()
        .map(|mut commands| {
            let ids = commands
                .iter()
                .filter_map(|(id, pending)| {
                    (pending.connection_generation == generation).then_some(*id)
                })
                .collect::<Vec<_>>();
            ids.into_iter()
                .filter_map(|id| commands.remove(&id).map(|command| (id, command)))
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    for (command_id, command) in pending {
        if let Ok(mut outcome) = command.outcome.lock() {
            *outcome = Some(Err(format!(
                "ENGINE_COMMAND_OUTCOME_UNKNOWN commandId={command_id}: {detail}"
            )));
            command.ready.notify_all();
        }
    }
}

fn status_supports_command_ack(shared: &PipeShared) -> bool {
    shared
        .latest_status
        .lock()
        .ok()
        .and_then(|status| status.clone())
        .and_then(|status| serde_json::from_str::<serde_json::Value>(&status).ok())
        .and_then(|status| {
            status
                .get("commandAckVersion")
                .and_then(serde_json::Value::as_u64)
        })
        == Some(1)
}

fn engine_command_ack_generation(shared: &PipeShared) -> Option<u64> {
    let Ok(_connection) = shared.connection.lock() else {
        return None;
    };
    let generation = shared.active_connection_generation.load(Ordering::Acquire);
    if generation == 0 || !status_supports_command_ack(shared) {
        return None;
    }
    Some(generation)
}

fn command_requires_ack(payload: &str) -> bool {
    serde_json::from_str::<serde_json::Value>(payload)
        .ok()
        .is_some_and(|command| {
            command.get("type").and_then(serde_json::Value::as_str)
                == Some("set-window-spatialization")
                && command
                    .pointer("/config/enabled")
                    .and_then(serde_json::Value::as_bool)
                    == Some(true)
        })
}

fn wait_for_command_ack_capability(shared: &Arc<PipeShared>) -> Result<Option<u64>, String> {
    if let Some(generation) = engine_command_ack_generation(shared) {
        return Ok(Some(generation));
    }
    // Establish the duplex pipe first: a newly started engine may not have
    // emitted its first status frame when bootstrap sends its first commands.
    let _ = connect_pipe(shared)?;
    for _ in 0..50 {
        if let Some(generation) = engine_command_ack_generation(shared) {
            return Ok(Some(generation));
        }
        thread::sleep(Duration::from_millis(10));
    }
    Ok(engine_command_ack_generation(shared))
}

fn write_pipe(shared: &Arc<PipeShared>, payload: &[u8]) -> Result<(), String> {
    let connection = connect_pipe(shared)?;
    write_connected_pipe(shared, &connection, payload)
}

fn write_connected_pipe(
    shared: &Arc<PipeShared>,
    connection: &EnginePipeConnection,
    payload: &[u8],
) -> Result<(), String> {
    let result = connection
        .writer
        .lock()
        .map_err(|_| "Verrou IPC empoisonné".to_string())?
        .write_all(payload)
        .map_err(|error| error.to_string());
    if result.is_err() {
        let _connection_guard = shared.connection.lock().ok();
        if let Ok(mut slot) = shared.writer.lock() {
            if slot
                .as_ref()
                .is_some_and(|current| Arc::ptr_eq(current, &connection.writer))
                && shared.active_connection_generation.load(Ordering::Acquire)
                    == connection.generation
            {
                *slot = None;
                shared
                    .active_connection_generation
                    .store(0, Ordering::Release);
                if let Ok(mut status) = shared.latest_status.lock() {
                    *status = None;
                }
            }
        }
    }
    result
}

fn connect_pose_pipe(shared: &PosePipeShared) -> Result<(), String> {
    let endpoint = shared.endpoint.as_ref().map_err(Clone::clone)?;
    let mut writer = shared
        .writer
        .lock()
        .map_err(|_| "Verrou du pipe pose empoisonné".to_string())?;
    if writer.is_none() {
        // Le serveur C++ expose ce pipe en entrée uniquement. Ne demander aucun
        // droit de lecture évite de recréer le couplage duplex commandes/statut.
        *writer = Some(
            OpenOptions::new()
                .write(true)
                .open(endpoint)
                .map_err(|error| format!("Connexion au pipe pose impossible : {error}"))?,
        );
    }
    Ok(())
}

fn write_connected_pose_pipe(shared: &PosePipeShared, packet: &[u8; 64]) -> Result<(), String> {
    let mut writer = shared
        .writer
        .lock()
        .map_err(|_| "Verrou du pipe pose empoisonné".to_string())?;
    let result = writer
        .as_mut()
        .ok_or_else(|| "Pipe pose non connecté".to_string())?
        .write_all(packet)
        .map_err(|error| format!("Écriture du pipe pose impossible : {error}"));
    if result.is_err() {
        // La pose fautive est abandonnée. La mailbox capacité 1 fournira la plus
        // récente au prochain tour, qui rouvrira alors une nouvelle connexion.
        *writer = None;
    }
    result
}

struct BuiltinHrtf {
    id: &'static str,
    output: &'static str,
    bytes: u64,
    sha256: &'static str,
}

const BUILTIN_HRTFS: [BuiltinHrtf; 6] = [
    BuiltinHrtf {
        id: "sadie-d2-kemar",
        output: "sadie-d2-kemar.sofa",
        bytes: 36_310_650,
        sha256: "bb4f3d4126e686f438c4dcd8c11541e62393c0d90db1da6f17d1dd13d01c8ef7",
    },
    BuiltinHrtf {
        id: "sadie-h6",
        output: "sadie-h6.sofa",
        bytes: 11_654_654,
        sha256: "785738cb0285a224d898d3f1cf84a81aaf6843c6ddefcdde7dcaaf9b772f5f80",
    },
    BuiltinHrtf {
        id: "sadie-h9",
        output: "sadie-h9.sofa",
        bytes: 11_654_654,
        sha256: "751ff726d7e2da72257357ffc910f0bae1cc88ee588f4b436a39f8c4af650c1f",
    },
    BuiltinHrtf {
        id: "sadie-h10",
        output: "sadie-h10.sofa",
        bytes: 8_753_949,
        sha256: "b48cf93b1b3919fa61a787242c75763fad2a4b0ee3464e53e165ba12a9720fde",
    },
    BuiltinHrtf {
        id: "sadie-h19",
        output: "sadie-h19.sofa",
        bytes: 8_753_949,
        sha256: "d28380500d40f428af702d90dbfdcef86328071796d45adf4a5822d88f49167c",
    },
    BuiltinHrtf {
        id: "sadie-h20",
        output: "sadie-h20.sofa",
        bytes: 8_753_949,
        sha256: "0377272defbf45f1f098c2fc0256fc4a9aa022545033ac9c80ea661403f61181",
    },
];

fn hash_file(path: &Path) -> Result<(u64, String), String> {
    let mut file = File::open(path).map_err(|error| error.to_string())?;
    let mut hasher = Sha256::new();
    let mut total = 0_u64;
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).map_err(|error| error.to_string())?;
        if read == 0 {
            break;
        }
        total = total
            .checked_add(read as u64)
            .ok_or_else(|| "Fichier trop volumineux".to_string())?;
        if total > MAX_SOFA_BYTES {
            return Err("Fichier SOFA trop volumineux".into());
        }
        hasher.update(&buffer[..read]);
    }
    Ok((total, hex::encode(hasher.finalize())))
}

fn validate_hashed_sofa(imported_root: &Path, candidate: &Path) -> Result<PathBuf, String> {
    let canonical_root = fs::canonicalize(imported_root)
        .map_err(|error| format!("Dossier HRTF local inaccessible : {error}"))?;
    let canonical = fs::canonicalize(candidate)
        .map_err(|error| format!("Fichier SOFA local inaccessible : {error}"))?;
    if !canonical.starts_with(&canonical_root) || canonical == canonical_root {
        return Err("Le profil SOFA doit provenir du stockage HRTF local de l’application".into());
    }
    if canonical
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| value.eq_ignore_ascii_case("sofa"))
        != Some(true)
    {
        return Err("Extension de profil SOFA invalide".into());
    }
    let expected_hash = canonical
        .file_stem()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "Nom de profil SOFA invalide".to_string())?;
    if expected_hash.len() != 64 || !expected_hash.bytes().all(|value| value.is_ascii_hexdigit()) {
        return Err("Le nom du profil SOFA ne contient pas son empreinte SHA-256".into());
    }
    let metadata = fs::metadata(&canonical).map_err(|error| error.to_string())?;
    if !metadata.is_file() || metadata.len() < 512 || metadata.len() > MAX_SOFA_BYTES {
        return Err("Taille de profil SOFA invalide".into());
    }
    let mut header = [0_u8; 8];
    File::open(&canonical)
        .and_then(|mut file| file.read_exact(&mut header))
        .map_err(|error| error.to_string())?;
    if header != [0x89, b'H', b'D', b'F', 0x0d, 0x0a, 0x1a, 0x0a] && &header[..3] != b"CDF" {
        return Err("Conteneur SOFA/HDF5 invalide".into());
    }
    let (bytes, actual_hash) = hash_file(&canonical)?;
    if bytes != metadata.len() || !actual_hash.eq_ignore_ascii_case(expected_hash) {
        return Err("L’empreinte du profil SOFA local ne correspond pas à son nom".into());
    }
    Ok(canonical)
}

fn builtin_hrtf_candidates(app: &AppHandle, output: &str) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Ok(resources) = app.path().resource_dir() {
        candidates.push(resources.join("hrtf").join("data").join(output));
    }
    if cfg!(debug_assertions) {
        candidates.push(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("resources/hrtf/data")
                .join(output),
        );
        candidates.push(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../../resources/hrtf/data")
                .join(output),
        );
    }
    candidates
}

fn resolve_builtin_hrtf(
    app: &AppHandle,
    state: &DesktopState,
    profile_id: &str,
) -> Result<Option<PathBuf>, String> {
    let Some(profile) = BUILTIN_HRTFS
        .iter()
        .find(|profile| profile.id == profile_id)
    else {
        return Ok(None);
    };
    if let Some(cached) = state
        .validated_hrtf
        .lock()
        .map_err(|_| "Verrou HRTF empoisonné".to_string())?
        .get(profile_id)
    {
        return Ok(Some(cached.clone()));
    }
    let path = builtin_hrtf_candidates(app, profile.output)
        .into_iter()
        .find(|candidate| candidate.is_file())
        .ok_or_else(|| {
            format!("Ressource HRTF {profile_id} absente. Lancez le script de ressources avant le packaging.")
        })?;
    let metadata = fs::metadata(&path).map_err(|error| error.to_string())?;
    if metadata.len() != profile.bytes {
        return Err(format!(
            "Ressource HRTF {profile_id} incomplète ({} octets au lieu de {}).",
            metadata.len(),
            profile.bytes
        ));
    }
    let mut file = File::open(&path).map_err(|error| error.to_string())?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).map_err(|error| error.to_string())?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    let digest = hex::encode(hasher.finalize());
    if digest != profile.sha256 {
        return Err(format!(
            "Empreinte SHA-256 invalide pour la ressource HRTF {profile_id}."
        ));
    }
    state
        .validated_hrtf
        .lock()
        .map_err(|_| "Verrou HRTF empoisonné".to_string())?
        .insert(profile_id.to_string(), path.clone());
    Ok(Some(path))
}

#[tauri::command]
fn get_builtin_hrtf_availability(
    app: AppHandle,
    state: State<'_, DesktopState>,
) -> Result<HashMap<String, bool>, String> {
    let mut availability = HashMap::with_capacity(BUILTIN_HRTFS.len());
    for profile in BUILTIN_HRTFS {
        let installed = state
            .validated_hrtf
            .lock()
            .map_err(|_| "Verrou HRTF empoisonné".to_string())?
            .contains_key(profile.id)
            || builtin_hrtf_candidates(&app, profile.output)
                .into_iter()
                .any(|candidate| {
                    fs::metadata(candidate)
                        .is_ok_and(|metadata| metadata.is_file() && metadata.len() == profile.bytes)
                });
        availability.insert(profile.id.to_string(), installed);
    }
    Ok(availability)
}

fn resolve_command_resources(
    app: &AppHandle,
    state: &DesktopState,
    payload: &str,
) -> Result<String, String> {
    let mut command: serde_json::Value = serde_json::from_str(payload)
        .map_err(|error| format!("Commande JSON invalide : {error}"))?;
    let command_type = command
        .get("type")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default();
    let (profile_id, sofa_path_pointer) = match command_type {
        "set-scene" => (
            command
                .pointer("/scene/hrtf/profileId")
                .and_then(serde_json::Value::as_str)
                .map(str::to_owned),
            "/scene/hrtf/sofaPath",
        ),
        "set-hrtf" => (
            command
                .get("profileId")
                .and_then(serde_json::Value::as_str)
                .map(str::to_owned),
            "/sofaPath",
        ),
        _ => return Ok(payload.to_string()),
    };
    let Some(profile_id) = profile_id else {
        return Err("Identifiant HRTF manquant dans la commande".into());
    };
    let existing = command
        .pointer(sofa_path_pointer)
        .and_then(serde_json::Value::as_str)
        .map(PathBuf::from);
    let resolved = if let Some(path) = resolve_builtin_hrtf(app, state, &profile_id)? {
        // Un profil intégré est toujours résolu depuis la ressource épinglée ; un chemin fourni
        // par le WebView ne peut donc pas substituer un fichier arbitraire.
        path
    } else {
        let hash_prefix = profile_id
            .strip_prefix("personal-")
            .filter(|value| value.len() == 8 && value.bytes().all(|byte| byte.is_ascii_hexdigit()))
            .ok_or_else(|| format!("Identifiant de profil importé invalide : {profile_id}"))?;
        let path = existing.ok_or_else(|| {
            format!("Le profil {profile_id} n’est ni intégré ni importé localement.")
        })?;
        let imported_root = app_data_dir(app)?.join("hrtf").join("imported");
        let canonical_requested = fs::canonicalize(&path)
            .map_err(|error| format!("Fichier SOFA local inaccessible : {error}"))?;
        let file_hash = canonical_requested
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        if !file_hash
            .get(..8)
            .is_some_and(|value| value.eq_ignore_ascii_case(hash_prefix))
        {
            return Err("L’identifiant du profil importé ne correspond pas à son empreinte".into());
        }
        let cached = state
            .validated_hrtf
            .lock()
            .map_err(|_| "Verrou HRTF empoisonné".to_string())?
            .get(&profile_id)
            .filter(|cached| **cached == canonical_requested)
            .cloned();
        if let Some(cached) = cached {
            cached
        } else {
            let validated = validate_hashed_sofa(&imported_root, &canonical_requested)?;
            state
                .validated_hrtf
                .lock()
                .map_err(|_| "Verrou HRTF empoisonné".to_string())?
                .insert(profile_id.clone(), validated.clone());
            validated
        }
    };
    *command
        .pointer_mut(sofa_path_pointer)
        .ok_or_else(|| "Champ sofaPath manquant".to_string())? =
        serde_json::Value::String(resolved.to_string_lossy().into_owned());
    serde_json::to_string(&command).map_err(|error| error.to_string())
}

#[tauri::command(async)]
fn send_engine_command(
    app: AppHandle,
    state: State<'_, DesktopState>,
    payload: String,
) -> Result<u64, String> {
    if payload.is_empty()
        || payload.len() > MAX_JSON_BYTES
        || serde_json::from_str::<serde_json::Value>(&payload).is_err()
    {
        return Err("Commande moteur invalide".into());
    }
    let payload = resolve_command_resources(&app, &state, &payload)?;
    if payload.is_empty() || payload.len() > MAX_JSON_BYTES {
        return Err("Commande moteur résolue trop volumineuse".into());
    }
    let requires_ack = command_requires_ack(&payload);
    let ack_generation = if requires_ack {
        wait_for_command_ack_capability(&state.pipe)?.ok_or_else(|| {
            "Le moteur actif est trop ancien pour la spatialisation par fenêtres. \
             Fermez complètement Sound Spatializer puis relancez l’application."
                .to_string()
        })?
    } else if let Some(generation) = engine_command_ack_generation(&state.pipe) {
        generation
    } else {
        // Compatibilité de mise à jour : un ancien moteur V1 est strict et
        // rejetterait le champ commandId. Il continue donc à recevoir la forme
        // historique jusqu'à son prochain redémarrage.
        let mut frame = Vec::with_capacity(4 + payload.len());
        frame.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        frame.extend_from_slice(payload.as_bytes());
        write_pipe(&state.pipe, &frame)?;
        return Ok(0);
    };
    let mut command = serde_json::from_str::<serde_json::Value>(&payload)
        .map_err(|error| format!("Commande moteur invalide : {error}"))?;
    let command_id = loop {
        let candidate = state.pipe.next_command_id.fetch_add(1, Ordering::Relaxed) as u32;
        if candidate != 0 {
            break candidate;
        }
    };
    command
        .as_object_mut()
        .ok_or_else(|| "La commande moteur doit être un objet JSON".to_string())?
        .insert("commandId".to_string(), serde_json::Value::from(command_id));
    let payload = serde_json::to_string(&command).map_err(|error| error.to_string())?;
    if payload.len() > MAX_JSON_BYTES {
        return Err("Commande moteur avec accusé de réception trop volumineuse".into());
    }
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    frame.extend_from_slice(payload.as_bytes());
    const REGISTRATION_ATTEMPTS: usize = 3;
    let mut registered = None;
    for _ in 0..REGISTRATION_ATTEMPTS {
        let connection = connect_pipe(&state.pipe)?;
        if engine_command_ack_generation(&state.pipe) != Some(connection.generation) {
            let refreshed_generation = wait_for_command_ack_capability(&state.pipe)?;
            if refreshed_generation != Some(connection.generation) {
                continue;
            }
        }
        let pending = Arc::new(PendingEngineCommand::new(connection.generation));
        let connection_guard = state
            .pipe
            .connection
            .lock()
            .map_err(|_| "Verrou de connexion IPC empoisonné".to_string())?;
        let still_current = state
            .pipe
            .active_connection_generation
            .load(Ordering::Acquire)
            == connection.generation
            && state
                .pipe
                .writer
                .lock()
                .map_err(|_| "Verrou IPC empoisonné".to_string())?
                .as_ref()
                .is_some_and(|writer| Arc::ptr_eq(writer, &connection.writer))
            && status_supports_command_ack(&state.pipe);
        if still_current {
            state
                .pipe
                .pending_commands
                .lock()
                .map_err(|_| "Verrou des réponses moteur empoisonné".to_string())?
                .insert(command_id, Arc::clone(&pending));
            drop(connection_guard);
            registered = Some((connection, pending));
            break;
        }
    }
    let (connection, pending) = registered.ok_or_else(|| {
        if requires_ack {
            format!(
                "Le moteur a changé pendant la négociation ACK (génération initiale {ack_generation}). \
                 Réessayez après sa reconnexion."
            )
        } else {
            "La connexion moteur a changé pendant l’envoi de la commande".to_string()
        }
    })?;
    if let Err(error) = write_connected_pipe(&state.pipe, &connection, &frame) {
        if let Ok(mut commands) = state.pipe.pending_commands.lock() {
            commands.remove(&command_id);
        }
        return Err(format!(
            "ENGINE_COMMAND_OUTCOME_UNKNOWN commandId={command_id}: écriture IPC incomplète ({error})"
        ));
    }

    let outcome = pending
        .outcome
        .lock()
        .map_err(|_| "Verrou de réponse moteur empoisonné".to_string())?;
    let (mut outcome, timeout) = pending
        .ready
        .wait_timeout_while(outcome, ENGINE_COMMAND_TIMEOUT, |value| value.is_none())
        .map_err(|_| "Attente de réponse moteur interrompue".to_string())?;
    if timeout.timed_out() && outcome.is_none() {
        if let Ok(mut commands) = state.pipe.pending_commands.lock() {
            commands.remove(&command_id);
        }
        return Err(format!(
            "ENGINE_COMMAND_OUTCOME_UNKNOWN commandId={command_id}: le moteur n’a pas confirmé la commande dans le délai imparti"
        ));
    }
    outcome
        .take()
        .unwrap_or_else(|| Err("Réponse moteur absente".to_string()))?;
    Ok(connection.generation)
}

fn pose_packet_from_invoke_body(body: &InvokeBody) -> Result<[u8; 64], String> {
    let payload = match body {
        InvokeBody::Raw(payload) => payload.as_slice(),
        InvokeBody::Json(_) => {
            return Err("Le paquet pose doit utiliser le transport binaire".into())
        }
    };
    payload
        .try_into()
        .map_err(|_| "Taille de paquet pose invalide".to_string())
}

#[tauri::command(async)]
fn push_head_pose(state: State<'_, DesktopState>, request: Request<'_>) -> Result<(), String> {
    let packet = pose_packet_from_invoke_body(request.body())?;
    if !validate_pose_packet(&packet) {
        return Err("Paquet HeadPoseSampleV1 invalide".into());
    }
    // Plusieurs invocations sont volontairement en vol côté WebView pour masquer
    // leur aller-retour. Une réponse tardive ne doit jamais réintroduire une pose
    // plus ancienne après une pose récente.
    let _ = state.pose_mailbox.publish(packet)?;
    Ok(())
}

fn validate_pose_packet(packet: &[u8; 64]) -> bool {
    if &packet[..4] != b"SSP1"
        || u16::from_le_bytes([packet[4], packet[5]]) != 1
        || u16::from_le_bytes([packet[6], packet[7]]) > 2
        || u32::from_le_bytes(packet[56..60].try_into().unwrap_or_default()) != 0
    {
        return false;
    }
    let supplied_crc = u32::from_le_bytes(packet[60..64].try_into().unwrap_or_default());
    if supplied_crc != 0 && supplied_crc != crc32(&packet[..60]) {
        return false;
    }
    let mut values = [0.0_f32; 8];
    for (index, value) in values.iter_mut().enumerate() {
        let offset = 24 + index * 4;
        *value = f32::from_le_bytes(packet[offset..offset + 4].try_into().unwrap_or_default());
    }
    let quaternion_norm = (values[0] * values[0]
        + values[1] * values[1]
        + values[2] * values[2]
        + values[3] * values[3])
        .sqrt();
    values.iter().all(|value| value.is_finite())
        && (0.8..=1.2).contains(&quaternion_norm)
        && values[4..7].iter().all(|value| value.abs() <= 50.0)
        && (0.0..=1.0).contains(&values[7])
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xffff_ffff_u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xedb8_8320 & mask);
        }
    }
    !crc
}

#[tauri::command]
fn get_engine_status(state: State<'_, DesktopState>) -> Result<Option<String>, String> {
    let _connection = state
        .pipe
        .connection
        .lock()
        .map_err(|_| "Verrou de connexion IPC empoisonné".to_string())?;
    let generation = state
        .pipe
        .active_connection_generation
        .load(Ordering::Acquire);
    let status = state
        .pipe
        .latest_status
        .lock()
        .map(|value| value.clone())
        .map_err(|_| "Verrou de statut empoisonné".to_string())?;
    status
        .map(|status| annotate_engine_status_generation(&status, generation))
        .transpose()
}

fn annotate_engine_status_generation(
    status: &str,
    connection_generation: u64,
) -> Result<String, String> {
    let mut value = serde_json::from_str::<serde_json::Value>(status)
        .map_err(|error| format!("Statut moteur JSON invalide : {error}"))?;
    value
        .as_object_mut()
        .ok_or_else(|| "Le statut moteur doit être un objet JSON".to_string())?
        .insert(
            "desktopConnectionGeneration".to_string(),
            serde_json::Value::from(connection_generation),
        );
    serde_json::to_string(&value).map_err(|error| error.to_string())
}

#[tauri::command]
fn open_windows_sound_settings() -> Result<(), String> {
    #[cfg(windows)]
    {
        Command::new("explorer.exe")
            .arg("ms-settings:sound")
            .spawn()
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

#[tauri::command]
fn qpc_snapshot() -> Result<QpcSnapshot, String> {
    #[cfg(windows)]
    unsafe {
        use windows::Win32::System::Performance::{
            QueryPerformanceCounter, QueryPerformanceFrequency,
        };
        let mut ticks = 0_i64;
        let mut frequency = 0_i64;
        QueryPerformanceCounter(&mut ticks).map_err(|error| error.to_string())?;
        QueryPerformanceFrequency(&mut frequency).map_err(|error| error.to_string())?;
        Ok(QpcSnapshot {
            ticks: ticks.to_string(),
            frequency: frequency.to_string(),
        })
    }
    #[cfg(not(windows))]
    {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|error| error.to_string())?
            .as_nanos();
        Ok(QpcSnapshot {
            ticks: nanos.to_string(),
            frequency: "1000000000".into(),
        })
    }
}

#[cfg(windows)]
fn windows_audio_devices() -> Result<Vec<AudioDeviceSummary>, String> {
    use windows::{
        core::{GUID, PWSTR},
        Win32::{
            Devices::FunctionDiscovery::PKEY_Device_FriendlyName,
            Foundation::{PROPERTYKEY, RPC_E_CHANGED_MODE},
            Media::Audio::*,
            System::Com::StructuredStorage::PropVariantToStringAlloc,
            System::Com::*,
            UI::Shell::PropertiesSystem::IPropertyStore,
        },
    };

    const PROPERTY_SET: GUID = GUID::from_values(
        0xb01e7f02,
        0x85b0,
        0x4cf9,
        [0xb5, 0x3d, 0x75, 0xdf, 0xd2, 0xb0, 0x5e, 0x07],
    );
    const ENDPOINT_MARKER: PROPERTYKEY = PROPERTYKEY {
        fmtid: PROPERTY_SET,
        pid: 2,
    };
    const CONTRACT_VERSION: PROPERTYKEY = PROPERTYKEY {
        fmtid: PROPERTY_SET,
        pid: 3,
    };

    struct ComApartment(bool);
    impl Drop for ComApartment {
        fn drop(&mut self) {
            if self.0 {
                unsafe { CoUninitialize() };
            }
        }
    }

    unsafe fn uint32_property(store: &IPropertyStore, key: &PROPERTYKEY) -> u32 {
        store
            .GetValue(key)
            .ok()
            .and_then(|value| u32::try_from(&value).ok())
            .unwrap_or(0)
    }

    unsafe fn mix_format_summary(device: &IMMDevice) -> (u32, u16, u32) {
        let Ok(client) = device.Activate::<IAudioClient>(CLSCTX_ALL, None) else {
            return (0, 0, 0);
        };
        let Ok(format) = client.GetMixFormat() else {
            return (0, 0, 0);
        };
        if format.is_null() {
            return (0, 0, 0);
        }
        let sample_rate = (*format).nSamplesPerSec;
        let channel_count = (*format).nChannels;
        let channel_mask = if (*format).wFormatTag == 0xfffe && (*format).cbSize as usize >= 22 {
            let extensible = format.cast::<WAVEFORMATEXTENSIBLE>();
            std::ptr::addr_of!((*extensible).dwChannelMask).read_unaligned()
        } else {
            match channel_count {
                1 => 0x4,
                2 => 0x3,
                // A legacy WAVEFORMATEX has no authoritative speaker map.
                // Never infer 5.1 from a bare six-channel count: the native
                // backend deliberately requires WAVEFORMATEXTENSIBLE too.
                _ => 0,
            }
        };
        CoTaskMemFree(Some(format.cast()));
        (sample_rate, channel_count, channel_mask)
    }

    unsafe {
        let com_result = CoInitializeEx(None, COINIT_MULTITHREADED);
        let _com = if com_result.is_ok() {
            ComApartment(true)
        } else if com_result == RPC_E_CHANGED_MODE {
            ComApartment(false)
        } else {
            return Err(format!("CoInitializeEx a échoué : {com_result:?}"));
        };
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)
                .map_err(|error| error.to_string())?;
        let default_id = enumerator
            .GetDefaultAudioEndpoint(eRender, eMultimedia)
            .ok()
            .and_then(|device| endpoint_id(&device).ok());
        let collection = enumerator
            .EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)
            .map_err(|error| error.to_string())?;
        let count = collection.GetCount().map_err(|error| error.to_string())?;
        let mut result = Vec::with_capacity(count as usize);
        for index in 0..count {
            let device = collection.Item(index).map_err(|error| error.to_string())?;
            let id = endpoint_id(&device)?;
            let store = device
                .OpenPropertyStore(STGM_READ)
                .map_err(|error| error.to_string())?;
            let value = store
                .GetValue(&PKEY_Device_FriendlyName)
                .map_err(|error| error.to_string())?;
            let name_ptr: PWSTR =
                PropVariantToStringAlloc(&value).map_err(|error| error.to_string())?;
            let name = name_ptr.to_string().map_err(|error| error.to_string())?;
            CoTaskMemFree(Some(name_ptr.as_ptr().cast()));
            let marker = uint32_property(&store, &ENDPOINT_MARKER);
            let contract_version = uint32_property(&store, &CONTRACT_VERSION);
            let upper = format!("{} {}", id, name).to_ascii_uppercase();
            let transport = if upper.contains("BTH") || upper.contains("BLUETOOTH") {
                "bluetooth"
            } else if upper.contains("USB") {
                "usb"
            } else if upper.contains("HDMI") || upper.contains("DISPLAY AUDIO") {
                "hdmi"
            } else {
                "unknown"
            };
            let (sample_rate, channel_count, channel_mask) = mix_format_summary(&device);
            result.push(AudioDeviceSummary {
                is_default: default_id.as_deref() == Some(id.as_str()),
                is_sound_spatializer_endpoint: is_sound_spatializer_endpoint(
                    marker,
                    contract_version,
                ),
                id,
                name,
                transport,
                sample_rate,
                channel_count,
                channel_mask,
            });
        }
        Ok(result)
    }
}

fn is_sound_spatializer_endpoint(endpoint_marker: u32, contract_version: u32) -> bool {
    endpoint_marker == 1 && contract_version == 1
}

#[cfg(windows)]
unsafe fn endpoint_id(device: &windows::Win32::Media::Audio::IMMDevice) -> Result<String, String> {
    use windows::Win32::System::Com::CoTaskMemFree;
    let pointer = device.GetId().map_err(|error| error.to_string())?;
    let value = pointer.to_string().map_err(|error| error.to_string())?;
    CoTaskMemFree(Some(pointer.as_ptr().cast()));
    Ok(value)
}

#[tauri::command(async)]
fn list_audio_devices() -> Result<Vec<AudioDeviceSummary>, String> {
    #[cfg(windows)]
    {
        windows_audio_devices()
    }
    #[cfg(not(windows))]
    {
        Ok(Vec::new())
    }
}

#[tauri::command(async)]
fn export_diagnostics(app: AppHandle, state: State<'_, DesktopState>) -> Result<String, String> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_secs();
    let directory = app_data_dir(&app)?.join("diagnostics");
    fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let path = directory.join(format!("diagnostic-{timestamp}.json"));
    let status = state
        .pipe
        .latest_status
        .lock()
        .map_err(|_| "Verrou de statut empoisonné".to_string())?
        .clone()
        .and_then(|value| serde_json::from_str::<serde_json::Value>(&value).ok());
    let document = serde_json::json!({
        "schemaVersion": 1,
        "generatedAtUnix": timestamp,
        "engineStatus": status,
        "poseTransport": state.pose_mailbox.diagnostics(),
        "privacy": "local-only"
    });
    let bytes = serde_json::to_vec_pretty(&document).map_err(|error| error.to_string())?;
    atomic_write(&path, &bytes)?;
    Ok(path.to_string_lossy().into_owned())
}

fn tray_icon_rgba() -> Vec<u8> {
    let mut pixels = vec![0_u8; 18 * 18 * 4];
    for y in 0..18 {
        for x in 0..18 {
            let index = (y * 18 + x) * 4;
            let border = x <= 1 || x >= 16 || y <= 1 || y >= 16;
            let wave = matches!(x, 6 | 9 | 12)
                && y >= (if x == 9 { 4 } else { 6 })
                && y <= (if x == 9 { 13 } else { 11 });
            if border || wave {
                pixels[index] = if wave { 104 } else { 25 };
                pixels[index + 1] = if wave { 229 } else { 45 };
                pixels[index + 2] = if wave { 207 } else { 54 };
                pixels[index + 3] = 255;
            }
        }
    }
    pixels
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            show_main_window(app);
        }))
        .plugin(tauri_plugin_dialog::init())
        .manage(DesktopState::default())
        .invoke_handler(tauri::generate_handler![
            load_app_config,
            save_app_config,
            import_sofa,
            import_headphone_eq,
            start_engine,
            send_engine_command,
            push_head_pose,
            get_engine_status,
            open_windows_sound_settings,
            qpc_snapshot,
            list_audio_devices,
            get_builtin_hrtf_availability,
            export_diagnostics
        ])
        .on_window_event(|window, event| {
            if window.label() == "main" {
                if let WindowEvent::CloseRequested { api, .. } = event {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .setup(|app| {
            let show = MenuItem::with_id(app, "show", "Afficher", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Quitter", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &quit])?;
            TrayIconBuilder::new()
                .icon(Image::new_owned(tray_icon_rgba(), 18, 18))
                .tooltip("Sound Spatializer")
                .menu(&menu)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => show_main_window(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main_window(tray.app_handle());
                    }
                })
                .build(app)?;
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("échec pendant la construction de Sound Spatializer")
        .run(|app, event| {
            // Couvre le menu Quitter comme toute autre sortie de la boucle
            // d'évènements : l'UI ne se ferme jamais en laissant le moteur
            // tenir le périphérique audio.
            if matches!(event, RunEvent::Exit) {
                shutdown_engine(&app.state::<DesktopState>());
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_pose_packet() -> [u8; 64] {
        let mut packet = [0_u8; 64];
        packet[..4].copy_from_slice(b"SSP1");
        packet[4..6].copy_from_slice(&1_u16.to_le_bytes());
        packet[6..8].copy_from_slice(&2_u16.to_le_bytes());
        packet[8..16].copy_from_slice(&1_u64.to_le_bytes());
        packet[16..24].copy_from_slice(&42_i64.to_le_bytes());
        packet[24..28].copy_from_slice(&1.0_f32.to_le_bytes());
        packet[52..56].copy_from_slice(&1.0_f32.to_le_bytes());
        let checksum = crc32(&packet[..60]);
        packet[60..64].copy_from_slice(&checksum.to_le_bytes());
        packet
    }

    #[test]
    fn sound_spatializer_endpoint_requires_vendor_marker() {
        assert!(is_sound_spatializer_endpoint(1, 1));
        assert!(!is_sound_spatializer_endpoint(0, 1));
        assert!(!is_sound_spatializer_endpoint(2, 1));
        assert!(!is_sound_spatializer_endpoint(1, 0));
        assert!(!is_sound_spatializer_endpoint(1, 2));
    }

    #[test]
    fn repository_release_resolves_sibling_then_workspace_engine() {
        let executable = Path::new(
            r"E:\repo\apps\desktop\src-tauri\target\release\sound-spatializer-desktop.exe",
        );
        let candidates = executable_engine_candidates(executable);
        assert_eq!(
            candidates[0],
            PathBuf::from(
                r"E:\repo\apps\desktop\src-tauri\target\release\SoundSpatializer.Engine.exe"
            )
        );
        assert_eq!(
            candidates[1],
            PathBuf::from(r"E:\repo\build\engine-mysofa\Release\SoundSpatializer.Engine.exe")
        );
        assert_eq!(
            candidates[2],
            PathBuf::from(r"E:\repo\build\engine\Release\SoundSpatializer.Engine.exe")
        );
    }

    #[test]
    fn repository_debug_prefers_the_fresh_workspace_engine() {
        let executable =
            Path::new(r"E:\repo\apps\desktop\src-tauri\target\debug\sound-spatializer-desktop.exe");
        let candidates = executable_engine_candidates(executable);
        assert_eq!(
            candidates[0],
            PathBuf::from(r"E:\repo\build\engine-mysofa\Debug\SoundSpatializer.Engine.exe")
        );
        assert_eq!(
            candidates[1],
            PathBuf::from(r"E:\repo\build\engine-dev\Debug\SoundSpatializer.Engine.exe")
        );
        assert_eq!(
            candidates[2],
            PathBuf::from(
                r"E:\repo\apps\desktop\src-tauri\target\debug\SoundSpatializer.Engine.exe"
            )
        );
    }

    #[test]
    fn installed_release_uses_only_the_wix_sibling_engine() {
        let executable = Path::new(r"C:\Program Files\Sound Spatializer\SoundSpatializer.exe");
        assert_eq!(
            executable_engine_candidates(executable),
            vec![PathBuf::from(
                r"C:\Program Files\Sound Spatializer\SoundSpatializer.Engine.exe"
            )]
        );
    }

    #[test]
    fn pipe_name_is_scoped_to_sid_and_session() {
        assert_eq!(
            format_engine_pipe_name("S-1-5-21-100-200-300-1001", 7).unwrap(),
            r"\\.\pipe\SoundSpatializer.Engine.v1.S-1-5-21-100-200-300-1001.7",
        );
        assert_eq!(
            format_pose_pipe_name("S-1-5-21-100-200-300-1001", 7).unwrap(),
            r"\\.\pipe\SoundSpatializer.Pose.v1.S-1-5-21-100-200-300-1001.7",
        );
        assert!(format_engine_pipe_name("../invalid", 7).is_err());
        assert!(format_pose_pipe_name("../invalid", 7).is_err());
    }

    #[test]
    fn command_result_frames_resolve_the_matching_waiter_without_replacing_status() {
        let shared = Arc::new(PipeShared::default());
        shared
            .active_connection_generation
            .store(1, Ordering::Release);
        let pending = Arc::new(PendingEngineCommand::new(1));
        shared
            .pending_commands
            .lock()
            .unwrap()
            .insert(42, Arc::clone(&pending));

        dispatch_engine_frame(
            r#"{"schemaVersion":1,"kind":"command-result","commandId":42,"accepted":false,"persisted":false,"error":"configuration refusée"}"#.to_string(),
            &shared,
            1,
        );

        assert_eq!(
            pending.outcome.lock().unwrap().take(),
            Some(Err("configuration refusée".to_string()))
        );
        assert!(shared.pending_commands.lock().unwrap().is_empty());
        assert!(shared.latest_status.lock().unwrap().is_none());

        dispatch_engine_frame(
            r#"{"schemaVersion":1,"captureState":"running"}"#.to_string(),
            &shared,
            1,
        );
        assert_eq!(
            shared.latest_status.lock().unwrap().as_deref(),
            Some(r#"{"schemaVersion":1,"captureState":"running"}"#)
        );
        assert!(engine_command_ack_generation(&shared).is_none());

        dispatch_engine_frame(
            r#"{"schemaVersion":1,"commandAckVersion":1,"captureState":"running"}"#.to_string(),
            &shared,
            1,
        );
        assert_eq!(engine_command_ack_generation(&shared), Some(1));

        let persistence_pending = Arc::new(PendingEngineCommand::new(1));
        shared
            .pending_commands
            .lock()
            .unwrap()
            .insert(43, Arc::clone(&persistence_pending));
        dispatch_engine_frame(
            r#"{"schemaVersion":1,"kind":"command-result","commandId":43,"accepted":true,"persisted":false,"error":"disque plein"}"#.to_string(),
            &shared,
            1,
        );
        assert_eq!(
            persistence_pending.outcome.lock().unwrap().take(),
            Some(Err(
                "ENGINE_COMMAND_APPLIED_NOT_PERSISTED commandId=43: disque plein".to_string()
            ))
        );
    }

    #[test]
    fn stale_reader_cannot_publish_or_resolve_for_a_new_generation() {
        let shared = Arc::new(PipeShared::default());
        shared
            .active_connection_generation
            .store(8, Ordering::Release);
        let current = Arc::new(PendingEngineCommand::new(8));
        shared
            .pending_commands
            .lock()
            .unwrap()
            .insert(80, Arc::clone(&current));

        dispatch_engine_frame(
            r#"{"schemaVersion":1,"captureState":"stale"}"#.to_string(),
            &shared,
            7,
        );
        dispatch_engine_frame(
            r#"{"schemaVersion":1,"kind":"command-result","commandId":80,"accepted":true,"persisted":true,"error":""}"#.to_string(),
            &shared,
            7,
        );

        assert!(shared.latest_status.lock().unwrap().is_none());
        assert!(current.outcome.lock().unwrap().is_none());
        assert!(shared.pending_commands.lock().unwrap().contains_key(&80));
    }

    #[test]
    fn window_spatialization_requires_an_ack_capable_engine() {
        assert!(command_requires_ack(
            r#"{"version":1,"type":"set-window-spatialization","config":{"enabled":true}}"#
        ));
        assert!(!command_requires_ack(
            r#"{"version":1,"type":"set-window-spatialization","config":{"enabled":false}}"#
        ));
        assert!(!command_requires_ack(
            r#"{"version":1,"type":"set-scene","scene":{}}"#
        ));
        assert!(!command_requires_ack("not json"));
    }

    #[test]
    fn desktop_connection_generation_is_attached_to_engine_status() {
        let annotated = annotate_engine_status_generation(
            r#"{"schemaVersion":1,"captureState":"running"}"#,
            27,
        )
        .unwrap();
        let value: serde_json::Value = serde_json::from_str(&annotated).unwrap();
        assert_eq!(
            value
                .get("desktopConnectionGeneration")
                .and_then(serde_json::Value::as_u64),
            Some(27)
        );
        assert!(annotate_engine_status_generation("[]", 1).is_err());
    }

    #[test]
    fn closing_an_old_pipe_generation_does_not_fail_new_commands() {
        let shared = PipeShared::default();
        let old = Arc::new(PendingEngineCommand::new(7));
        let current = Arc::new(PendingEngineCommand::new(8));
        {
            let mut commands = shared.pending_commands.lock().unwrap();
            commands.insert(70, Arc::clone(&old));
            commands.insert(80, Arc::clone(&current));
        }

        fail_pending_engine_commands_for_generation(&shared, 7, "ancienne connexion interrompue");

        assert_eq!(
            old.outcome.lock().unwrap().take(),
            Some(Err(
                "ENGINE_COMMAND_OUTCOME_UNKNOWN commandId=70: ancienne connexion interrompue"
                    .to_string()
            ))
        );
        assert!(current.outcome.lock().unwrap().is_none());
        let commands = shared.pending_commands.lock().unwrap();
        assert!(!commands.contains_key(&70));
        assert!(commands.contains_key(&80));
    }

    #[test]
    fn validates_pose_packet_crc_and_finite_values() {
        let packet = valid_pose_packet();
        assert!(validate_pose_packet(&packet));

        let mut corrupted = packet;
        corrupted[28] ^= 0x40;
        assert!(!validate_pose_packet(&corrupted));

        let mut non_finite = valid_pose_packet();
        non_finite[40..44].copy_from_slice(&f32::NAN.to_le_bytes());
        let checksum = crc32(&non_finite[..60]);
        non_finite[60..64].copy_from_slice(&checksum.to_le_bytes());
        assert!(!validate_pose_packet(&non_finite));
    }

    #[test]
    fn pose_invoke_requires_an_exact_raw_binary_packet() {
        let packet = valid_pose_packet();
        assert_eq!(
            pose_packet_from_invoke_body(&InvokeBody::Raw(packet.to_vec())).unwrap(),
            packet
        );
        assert!(pose_packet_from_invoke_body(&InvokeBody::Raw(vec![0_u8; 63])).is_err());
        assert!(
            pose_packet_from_invoke_body(&InvokeBody::Json(serde_json::json!({
                "payload": packet.to_vec()
            })))
            .is_err()
        );
    }

    #[test]
    fn pose_mailbox_discards_late_out_of_order_invocations() {
        let mailbox = PoseMailbox::default();
        let mut newest = valid_pose_packet();
        newest[8..16].copy_from_slice(&7_u64.to_le_bytes());
        let checksum = crc32(&newest[..60]);
        newest[60..64].copy_from_slice(&checksum.to_le_bytes());
        assert!(mailbox.publish(newest).unwrap());

        let mut late = valid_pose_packet();
        late[8..16].copy_from_slice(&6_u64.to_le_bytes());
        let checksum = crc32(&late[..60]);
        late[60..64].copy_from_slice(&checksum.to_le_bytes());
        assert!(!mailbox.publish(late).unwrap());

        let state = mailbox.state.lock().unwrap();
        let retained = state.latest.as_ref().unwrap();
        assert_eq!(u64::from_le_bytes(retained[8..16].try_into().unwrap()), 7);
        assert_eq!(state.highest_sequence, 7);
    }

    #[test]
    fn config_requires_both_schema_versions_and_preferences() {
        assert!(validate_app_config_json(
            r#"{"schemaVersion":1,"scene":{"schemaVersion":1},"preferences":{}}"#
        )
        .is_ok());
        assert!(validate_app_config_json(
            r#"{"schemaVersion":2,"scene":{"schemaVersion":2},"preferences":{}}"#
        )
        .is_ok());
        assert!(validate_app_config_json(
            r#"{"schemaVersion":3,"scene":{"schemaVersion":2},"preferences":{},"windowSpatialization":{}}"#
        )
        .is_ok());
        assert!(validate_app_config_json(
            r#"{"schemaVersion":3,"scene":{"schemaVersion":2},"preferences":{}}"#
        )
        .is_err());
        assert!(validate_app_config_json(
            r#"{"schemaVersion":1,"scene":{"schemaVersion":2},"preferences":{}}"#
        )
        .is_err());
        assert!(validate_app_config_json(
            r#"{"schemaVersion":2,"scene":{"schemaVersion":1},"preferences":{}}"#
        )
        .is_err());
        assert!(
            validate_app_config_json(r#"{"schemaVersion":1,"scene":{"schemaVersion":1}}"#).is_err()
        );
    }

    #[test]
    fn imported_sofa_must_be_content_addressed_inside_local_root() {
        let base = std::env::temp_dir().join(format!(
            "sound-spatializer-ui-test-{}-{}",
            std::process::id(),
            TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed),
        ));
        let imported = base.join("imported");
        fs::create_dir_all(&imported).expect("create test directory");
        let mut bytes = vec![0_u8; 512];
        bytes[..8].copy_from_slice(&[0x89, b'H', b'D', b'F', 0x0d, 0x0a, 0x1a, 0x0a]);
        let hash = hex::encode(Sha256::digest(&bytes));
        let valid = imported.join(format!("{hash}.sofa"));
        fs::write(&valid, &bytes).expect("write SOFA fixture");
        assert_eq!(
            validate_hashed_sofa(&imported, &valid).expect("valid fixture"),
            fs::canonicalize(&valid).unwrap()
        );

        let outside = base.join(format!("{hash}.sofa"));
        fs::write(&outside, &bytes).expect("write outside fixture");
        assert!(validate_hashed_sofa(&imported, &outside).is_err());
        fs::remove_dir_all(&base).expect("remove test directory");
    }

    #[test]
    fn parses_strict_json_eq_v1_and_clamps_contract_ranges() {
        let parsed = parse_headphone_eq_json(
            r#"{
                "format":"sound-spatializer-headphone-eq",
                "schemaVersion":1,
                "profileName":"Casque de référence",
                "preampDb":-90,
                "filters":[
                    {"enabled":true,"type":"peak","frequencyHz":5,"gainDb":30,"q":0.001},
                    {"enabled":false,"type":"high-shelf","frequencyHz":30000,"gainDb":-30,"q":40}
                ]
            }"#,
            "reference.json",
        )
        .expect("valid JSON EQ");
        assert_eq!(parsed.format, "sound-spatializer-json-v1");
        assert_eq!(parsed.profile_name, "Casque de référence");
        assert_eq!(parsed.preamp_db, -24.0);
        assert_eq!(parsed.bands.len(), 2);
        assert_eq!(parsed.bands[0].frequency_hz, 10.0);
        assert_eq!(parsed.bands[0].gain_db, 24.0);
        assert_eq!(parsed.bands[0].q, 0.01);
        assert_eq!(parsed.bands[1].frequency_hz, 24_000.0);
        assert!(parse_headphone_eq_json(
            r#"{"format":"sound-spatializer-headphone-eq","schemaVersion":1,"profileName":"X","preampDb":-6,"filters":[{"enabled":true,"type":"peak","frequencyHz":1000,"gainDb":0,"q":1,"unknown":1}]}"#,
            "strict.json",
        )
        .is_err());
    }

    #[test]
    fn json_eq_rejects_more_than_sixteen_filters() {
        let filters = (0..17)
            .map(|_| {
                serde_json::json!({
                    "enabled": true,
                    "type": "peak",
                    "frequencyHz": 1000,
                    "gainDb": 0,
                    "q": 1
                })
            })
            .collect::<Vec<_>>();
        let document = serde_json::json!({
            "format": "sound-spatializer-headphone-eq",
            "schemaVersion": 1,
            "profileName": "Trop de filtres",
            "preampDb": -6,
            "filters": filters
        });
        assert!(parse_headphone_eq_json(&document.to_string(), "too-many.json").is_err());
    }

    #[test]
    fn parses_equalizer_apo_autoeq_and_rejects_unknown_directives() {
        let parsed = parse_equalizer_apo(
            "# AutoEQ local\nPreamp: -6.5 dB\nFilter 1: ON PK Fc 105 Hz Gain 3.2 dB Q 0.70\nFilter 2: OFF LSC Fc 80 Hz Gain -2 dB Q 1.0\nFilter 3: ON HSC Fc 9000 Hz Gain 1 dB Q 0.7\n",
            "headphones.txt",
            "headphones",
        )
        .expect("valid Equalizer APO profile");
        assert_eq!(parsed.format, "equalizer-apo");
        assert_eq!(parsed.preamp_db, -6.5);
        assert_eq!(parsed.bands.len(), 3);
        assert_eq!(parsed.bands[0].filter_type, "peak");
        assert_eq!(parsed.bands[1].filter_type, "low-shelf");
        assert!(!parsed.bands[1].enabled);
        assert_eq!(parsed.bands[2].filter_type, "high-shelf");
        assert!(
            parse_equalizer_apo("Preamp: -6 dB\nGraphicEQ: 20 0; 1000 -2", "bad.txt", "bad",)
                .is_err()
        );
    }

    #[test]
    fn local_eq_reader_enforces_one_mebibyte_limit() {
        let base = std::env::temp_dir().join(format!(
            "sound-spatializer-eq-test-{}-{}",
            std::process::id(),
            TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed),
        ));
        fs::create_dir_all(&base).expect("create EQ test directory");
        let oversized = base.join("oversized.txt");
        fs::write(&oversized, vec![b' '; MAX_EQ_BYTES as usize + 1]).expect("write oversized EQ");
        assert!(read_headphone_eq(&oversized).is_err());
        fs::remove_dir_all(&base).expect("remove EQ test directory");
    }
}
