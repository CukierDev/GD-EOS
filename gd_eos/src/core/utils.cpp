#include <core/utils.h>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows/eos_Windows.h>
#elif defined(__ANDROID__)
#include <Android/eos_android.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#endif

namespace godot::eos {

#ifdef DEBUG_ENABLED
String eos_product_user_id_to_string(EOS_ProductUserId p_product_user_id) {
    char buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1];
    int32_t inoutlength{ EOS_PRODUCTUSERID_MAX_LENGTH + 1 };
    EOS_ProductUserId_ToString(p_product_user_id, buffer, &inoutlength);
    return buffer;
}
String eos_epic_account_id_to_string(EOS_EpicAccountId p_epic_account_id) {
    char buffer[EOS_EPICACCOUNTID_MAX_LENGTH + 1];
    int32_t inoutlength{ EOS_EPICACCOUNTID_MAX_LENGTH + 1 };
    EOS_EpicAccountId_ToString(p_epic_account_id, buffer, &inoutlength);
    return buffer;
}
#endif // DEBUG_ENABLED

#define EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_INTERNAL_DIRECTORY "GD_EOS/platforms/android/optional_internal_directory"
#define EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_EXTERNAL_DIRECTORY "GD_EOS/platforms/android/optional_external_directory"

#if defined(TOOLS_ENABLED) || defined(DEV_ENABLED) || defined(DEBUG_ENABLED)
void setup_eos_project_settings() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    ps->set_setting(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_INTERNAL_DIRECTORY, "user://");
    ps->set_setting(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_EXTERNAL_DIRECTORY, "");

    ps->set_initial_value(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_INTERNAL_DIRECTORY, "user://");
    ps->set_initial_value(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_EXTERNAL_DIRECTORY, "");
}
#endif // defined(TOOLS_ENABLED) || defined(DEV_ENABLED) || defined(DEBUG_ENABLED)

void *get_platform_specific_options() {
#if defined(_WIN32) || defined(_WIN64)
    static struct Wrapper {
        EOS_Windows_RTCOptions windowsRTCOptions{};

        void set_XAudio29DllPath(const String &p_XAudio29DllPath) {
            XAudio29DllPath = {};
            windowsRTCOptions.XAudio29DllPath = nullptr;
            if (!p_XAudio29DllPath.is_empty()) {
                XAudio29DllPath = p_XAudio29DllPath.utf8();
                windowsRTCOptions.XAudio29DllPath = XAudio29DllPath.get_data();
            }
        }

    private:
        CharString XAudio29DllPath{};
    } wrapper{};

    wrapper.windowsRTCOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
    String xAudio29DllPath;
    if (OS::get_singleton()->has_feature("editor")) {
        String bin_path = "res://addons/gd_eos/bin/";
// Super janky solution
#if defined(_WIN64)
    xAudio29DllPath = "C:\\Windows\\System32\\XAudio2_9.dll";
#else
    xAudio29DllPath = "C:\\Windows\\SysWOW64\\XAudio2_9.dll";
#endif
    } else {
        xAudio29DllPath = OS::get_singleton()->get_executable_path().get_base_dir().path_join("xaudio2_9redist.dll");
    }
    wrapper.set_XAudio29DllPath(xAudio29DllPath);
    return &wrapper.windowsRTCOptions;
#else
    return nullptr;
#endif
}

void *get_system_initialize_options() {
#if defined(__ANDROID__)
    static struct Wrapper {
        EOS_Android_InitializeOptions androidInitializeOptions{};
        void set_InternalDirectory(const String &p_internal_directory) {
            androidInitializeOptions.OptionalInternalDirectory = nullptr;
            InternalDirectory = {};
            if (!p_internal_directory.is_empty()) {
                InternalDirectory = p_internal_directory.utf8();
                androidInitializeOptions.OptionalInternalDirectory = InternalDirectory.get_data();
            }
        }
        void set_ExternalDirectory(const String &p_external_directory) {
            androidInitializeOptions.OptionalExternalDirectory = nullptr;
            ExternalDirectory = {};
            if (!p_external_directory.is_empty()) {
                ExternalDirectory = p_external_directory.utf8();
                androidInitializeOptions.OptionalExternalDirectory = ExternalDirectory.get_data();
            }
        }

    private:
        CharString InternalDirectory{};
        CharString ExternalDirectory{};
    } wrapper{};

    String internal_dir{};
    String external_dir{};
    Variant internal_var{};
    Variant external_var{};

    if (ProjectSettings::get_singleton()->has_setting(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_INTERNAL_DIRECTORY)) {
        internal_var = ProjectSettings::get_singleton()->get_setting_with_override(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_INTERNAL_DIRECTORY);
    }
    if (ProjectSettings::get_singleton()->has_setting(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_EXTERNAL_DIRECTORY)) {
        external_var = ProjectSettings::get_singleton()->get_setting_with_override(EOS_PLATFORM_SPECIFIC_SETTING_ANDROID_EXTERNAL_DIRECTORY);
    }

    if (internal_var.get_type() == Variant::STRING) {
        String path = internal_var;
        if (!path.is_empty()) {
            path = ProjectSettings::get_singleton()->globalize_path(path);
            if (!DirAccess::dir_exists_absolute(path)) {
                DirAccess::make_dir_absolute(path);
            }

            if (DirAccess::dir_exists_absolute(path)) {
                internal_dir = path;
            } else {
                ERR_PRINT(vformat("EOS warning: \"%s\" is not a valid directory.", internal_var));
            }
        }
    } else if (internal_var.get_type() != Variant::NIL) {
        ERR_PRINT(vformat("EOS warning: \"%s\" is not a valid directory.", internal_var));
    }

    if (external_var.get_type() == Variant::STRING && !String(external_var).is_empty()) {
        String path = external_var;
        if (!path.is_empty()) {
            path = ProjectSettings::get_singleton()->globalize_path(path);
            if (!DirAccess::dir_exists_absolute(path)) {
                DirAccess::make_dir_absolute(path);
            }

            if (DirAccess::dir_exists_absolute(path)) {
                external_dir = path;
            } else {
                ERR_PRINT(vformat("EOS warning: \"%s\" is not a valid directory.", external_var));
            }
        }
    } else if (external_var.get_type() != Variant::NIL) {
        ERR_PRINT(vformat("EOS warning: \"%s\" is not a valid directory.", external_var));
    }

    if (internal_dir.is_empty()) {
        internal_dir = ProjectSettings::get_singleton()->globalize_path(OS::get_singleton()->get_user_data_dir());
    }

    wrapper.androidInitializeOptions.ApiVersion = EOS_ANDROID_INITIALIZEOPTIONS_API_LATEST;
    wrapper.androidInitializeOptions.Reserved = nullptr;
    wrapper.set_InternalDirectory(internal_dir);
    wrapper.set_ExternalDirectory(external_dir);
    return &wrapper.androidInitializeOptions;
#else
    return nullptr;
#endif
}
} //namespace godot::eos