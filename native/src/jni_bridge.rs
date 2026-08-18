//! JNI adapter for the Android Kotlin bridge.
//!
//! The PTY model itself lives in `session`; this file only converts JNI values
//! and marshals native worker-thread events onto the JVM.

use crate::{
    ChrootSpec, DroidspacesSpec, LaunchSpec, ProotSpec, SessionEvents, SessionId, SessionManager,
    SessionResult,
};
use jni::objects::{GlobalRef, JByteArray, JObject, JObjectArray, JString, JValue};
use jni::sys::{jboolean, jlong};
use jni::{JNIEnv, JavaVM};
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};

static SESSIONS: OnceLock<SessionManager> = OnceLock::new();

fn sessions() -> &'static SessionManager {
    SESSIONS.get_or_init(SessionManager::new)
}

struct JvmEvents {
    vm: JavaVM,
    callback: GlobalRef,
}

impl SessionEvents for JvmEvents {
    fn on_output(&self, session_id: SessionId, bytes: &[u8]) {
        let Ok(mut env) = self.vm.attach_current_thread_as_daemon() else {
            return;
        };
        let Ok(array) = env.byte_array_from_slice(bytes) else {
            return;
        };
        let array = JObject::from(array);
        let _ = env.call_method(
            self.callback.as_obj(),
            "onSessionOutput",
            "(J[B)V",
            &[JValue::Long(session_id), JValue::Object(&array)],
        );
        clear_exception(&mut env);
    }

    fn on_exited(&self, session_id: SessionId, result: SessionResult) {
        let Ok(mut env) = self.vm.attach_current_thread_as_daemon() else {
            return;
        };
        let _ = env.call_method(
            self.callback.as_obj(),
            "onSessionExited",
            "(JI)V",
            &[JValue::Long(session_id), JValue::Int(result.exit_code())],
        );
        clear_exception(&mut env);
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_openSession(
    mut env: JNIEnv,
    _: JObject,
    command: JString,
    arguments: JObjectArray,
    working_directory: JString,
    environment: JObjectArray,
    rows: i32,
    columns: i32,
    callback: JObject,
) -> jlong {
    let result = (|| {
        let spec = LaunchSpec {
            command: PathBuf::from(java_string(&mut env, command)?),
            arguments: java_string_array(&mut env, arguments)?,
            working_directory: PathBuf::from(java_string(&mut env, working_directory)?),
            environment: java_string_array(&mut env, environment)?,
        };
        let events = Arc::new(JvmEvents {
            vm: env.get_java_vm().map_err(|error| error.to_string())?,
            callback: env
                .new_global_ref(callback)
                .map_err(|error| error.to_string())?,
        });
        sessions()
            .open(spec, rows, columns, events)
            .map_err(|error| error.to_string())
    })();
    match result {
        Ok(id) => id,
        Err(message) => {
            throw_illegal_argument(&mut env, &message);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_openProotSession(
    mut env: JNIEnv,
    _: JObject,
    rootfs: JString,
    shell: JString,
    native_library_directory: JString,
    cache_directory: JString,
    x11_socket_directory: JString,
    launch_argv: JObjectArray,
    rows: i32,
    columns: i32,
    callback: JObject,
) -> jlong {
    let result = (|| {
        let spec = ProotSpec {
            rootfs: PathBuf::from(java_string(&mut env, rootfs)?),
            shell: PathBuf::from(java_string(&mut env, shell)?),
            native_library_dir: PathBuf::from(java_string(&mut env, native_library_directory)?),
            cache_dir: PathBuf::from(java_string(&mut env, cache_directory)?),
            x11_socket_directory: PathBuf::from(java_string(&mut env, x11_socket_directory)?),
            launch_argv: java_string_array(&mut env, launch_argv)?,
        };
        let events = Arc::new(JvmEvents {
            vm: env.get_java_vm().map_err(|error| error.to_string())?,
            callback: env
                .new_global_ref(callback)
                .map_err(|error| error.to_string())?,
        });
        sessions()
            .open_proot(spec, rows, columns, events)
            .map_err(|error| error.to_string())
    })();
    match result {
        Ok(id) => id,
        Err(message) => {
            throw_illegal_argument(&mut env, &message);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_openChrootSession(
    mut env: JNIEnv,
    _: JObject,
    rootfs: JString,
    shell: JString,
    x11_socket_directory: JString,
    launch_argv: JObjectArray,
    rows: i32,
    columns: i32,
    callback: JObject,
) -> jlong {
    let result = (|| {
        let spec = ChrootSpec {
            rootfs: PathBuf::from(java_string(&mut env, rootfs)?),
            shell: PathBuf::from(java_string(&mut env, shell)?),
            x11_socket_directory: PathBuf::from(java_string(&mut env, x11_socket_directory)?),
            launch_argv: java_string_array(&mut env, launch_argv)?,
        };
        let events = Arc::new(JvmEvents {
            vm: env.get_java_vm().map_err(|error| error.to_string())?,
            callback: env
                .new_global_ref(callback)
                .map_err(|error| error.to_string())?,
        });
        sessions()
            .open_chroot(spec, rows, columns, events)
            .map_err(|error| error.to_string())
    })();
    match result {
        Ok(id) => id,
        Err(message) => {
            throw_illegal_argument(&mut env, &message);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_openDroidspacesSession(
    mut env: JNIEnv,
    _: JObject,
    container: JString,
    user: JString,
    x11_socket_directory: JString,
    launch_argv: JObjectArray,
    rows: i32,
    columns: i32,
    callback: JObject,
) -> jlong {
    let result = (|| {
        let spec = DroidspacesSpec {
            container: java_string(&mut env, container)?,
            user: java_string(&mut env, user)?,
            x11_socket_directory: java_string(&mut env, x11_socket_directory)?,
            launch_argv: java_string_array(&mut env, launch_argv)?,
        };
        let events = Arc::new(JvmEvents {
            vm: env.get_java_vm().map_err(|error| error.to_string())?,
            callback: env
                .new_global_ref(callback)
                .map_err(|error| error.to_string())?,
        });
        sessions()
            .open(
                spec.launch_spec().map_err(|error| error.to_string())?,
                rows,
                columns,
                events,
            )
            .map_err(|error| error.to_string())
    })();
    match result {
        Ok(id) => id,
        Err(message) => {
            throw_illegal_argument(&mut env, &message);
            0
        }
    }
}

/// Repairs only Trierarch's private X11 socket directory after an old build
/// bind-mounted that *directory* into a guest, letting the guest change its
/// ownership.  New builds bind the X0 socket file only, so this is migration
/// recovery rather than part of the normal launch path.
#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_repairX11SocketDirectory(
    mut env: JNIEnv,
    _: JObject,
    directory: JString,
    app_uid: i32,
) {
    let result = (|| {
        if app_uid < 0 {
            return Err("invalid application uid".to_owned());
        }
        let directory = java_string(&mut env, directory)?;
        let path = PathBuf::from(&directory);
        if !path.is_absolute()
            || path.file_name().and_then(|name| name.to_str()) != Some(".X11-unix")
        {
            return Err("invalid X11 socket directory".to_owned());
        }

        let quoted_directory = crate::privileged::shell_quote(&directory);
        let command = format!(
            "mkdir -p {quoted_directory} && chown {app_uid}:{app_uid} {quoted_directory} && chmod 1777 {quoted_directory} && if [ ! -S {quoted_directory}/X0 ]; then rm -f {quoted_directory}/.X0-lock; fi"
        );
        crate::privileged::run_as_root(command).map_err(|error| error.to_string())
    })();
    if let Err(message) = result {
        throw_illegal_argument(&mut env, &message);
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_isDroidspacesRunning(
    mut env: JNIEnv,
    _: JObject,
    container: JString,
) -> jboolean {
    match java_string(&mut env, container).and_then(|container| {
        DroidspacesSpec::is_container_running(&container).map_err(|error| error.to_string())
    }) {
        Ok(running) => running as jboolean,
        Err(message) => {
            throw_illegal_argument(&mut env, &message);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_stopDroidspaces(
    mut env: JNIEnv,
    _: JObject,
    container: JString,
) {
    let result = java_string(&mut env, container).and_then(|container| {
        DroidspacesSpec::stop_container(&container).map_err(|error| error.to_string())
    });
    if let Err(message) = result {
        throw_illegal_argument(&mut env, &message);
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_write(
    env: JNIEnv,
    _: JObject,
    session_id: jlong,
    bytes: JByteArray,
) -> jboolean {
    let Ok(bytes) = env.convert_byte_array(&bytes) else {
        return 0;
    };
    sessions().write(session_id, &bytes).is_ok() as jboolean
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_resize(
    _: JNIEnv,
    _: JObject,
    session_id: jlong,
    rows: i32,
    columns: i32,
) -> jboolean {
    sessions().resize(session_id, rows, columns).is_ok() as jboolean
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_app_trierarch_nativebridge_NativePtyBridge_close(
    _: JNIEnv,
    _: JObject,
    session_id: jlong,
) {
    sessions().close(session_id);
}

fn java_string(env: &mut JNIEnv, value: JString) -> Result<String, String> {
    env.get_string(&value)
        .map(|value| value.into())
        .map_err(|error| error.to_string())
}

fn java_string_array(env: &mut JNIEnv, array: JObjectArray) -> Result<Vec<String>, String> {
    let length = env
        .get_array_length(&array)
        .map_err(|error| error.to_string())?;
    (0..length)
        .map(|index| {
            let item = env
                .get_object_array_element(&array, index)
                .map_err(|error| error.to_string())?;
            java_string(env, JString::from(item))
        })
        .collect()
}

fn throw_illegal_argument(env: &mut JNIEnv, message: &str) {
    let _ = env.throw_new("java/lang/IllegalArgumentException", message);
}

fn clear_exception(env: &mut JNIEnv) {
    if env.exception_check().unwrap_or(false) {
        let _ = env.exception_clear();
    }
}
