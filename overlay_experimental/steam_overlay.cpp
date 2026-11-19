#ifdef EMU_OVERLAY

// if you're wondering about text like: ##PopupAcceptInvite
// these are unique labels (keys) for each button/label/text,etc...
// ImGui uses the labels as keys, adding a suffic like "My Text##SomeKey"
// avoids confusing ImGui when another label has the same text "MyText"

#include "overlay/steam_overlay.h"

#include <thread>
#include <string>
#include <sstream>
#include <cctype>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <sys/stat.h>
#include <cstdlib>  // std::system

#ifdef __WINDOWS__
#include <shellapi.h>
#endif

#include "InGameOverlay/RendererDetector.h"

#include "dll/dll.h"
#include "dll/settings_parser.h"
#include "dll/screenshot_format.h"

// image loading for gallery thumbnails
// local_storage.cpp already defines the IMPLEMENTATION with _STATIC,
// so we must do the same here to avoid linker errors (each .obj gets its own private copy).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

// translation
#include "overlay/steam_overlay_translations.h"
// fonts
#include "fonts/unifont.hpp"
// builtin audio
#include "overlay/notification.h"

#include "overlay/persian_shaper.hpp"

#define URL_WINDOW_NAME "URL Window"

static constexpr int max_window_id = 10000;
static constexpr int base_notif_window_id  = 0 * max_window_id;
static constexpr int base_friend_window_id = 1 * max_window_id;
static constexpr int base_friend_item_id   = 2 * max_window_id;

static constexpr int PERSIAN_LANG_INDEX = 31;

static constexpr const char* valid_languages[] = {
    "english",
    "arabic",
    "bulgarian",
    "schinese",
    "tchinese",
    "czech",
    "danish",
    "dutch",
    "finnish",
    "french",
    "german",
    "greek",
    "hungarian",
    "italian",
    "japanese",
    "koreana",
    "norwegian",
    "polish",
    "portuguese",
    "brazilian",
    "romanian",
    "russian",
    "spanish",
    "latam",
    "swedish",
    "thai",
    "turkish",
    "ukrainian",
    "vietnamese",
    "croatian",
    "indonesian",
    "persian",
};

// تبدیل ایمن متون فارسی به فرم چسبیده و راست‌به‌چپ (بدون دوباره‌کاری روی رشته شکل‌یافته)
static inline std::string fa_shape(const std::string& s) {
    auto cps = PersianShaper::utf8_to_codepoints(s);
    for (uint32_t cp : cps) {
        if (cp >= 0x0600 && cp <= 0x06FF) {
            return PersianShaper::shape_for_overlay(s);
        }
    }
    return s;
}

static const char* overlay_label(int language, const char* english, const char* persian)
{
    if (language != PERSIAN_LANG_INDEX) return english;

    static thread_local std::unordered_map<const char*, std::string> shaped_labels;
    auto it = shaped_labels.find(persian);
    if (it == shaped_labels.end()) {
        it = shaped_labels.emplace(persian, fa_shape(persian)).first;
    }
    return it->second.c_str();
}

template <typename T>
static inline const char* tr_ui(int lang, const T& table, const char* fa_raw)
{
    const int safe_idx = (lang >= 0 && lang < PERSIAN_LANG_INDEX) ? lang : 0;
    return overlay_label(lang, table[safe_idx], fa_raw);
}

static void align_overlay_item_right(float width)
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
}

static void overlay_line(const char* visual_text, bool rtl)
{
    if (rtl) align_overlay_item_right(ImGui::CalcTextSize(visual_text).x);
    ImGui::TextUnformatted(visual_text);
}

static void overlay_paragraph(const char* text, bool rtl)
{
    if (!rtl) {
        ImGui::TextWrapped("%s", text);
        return;
    }

    const float width = ImGui::GetContentRegionAvail().x;
    std::istringstream paragraphs(text);
    for (std::string paragraph; std::getline(paragraphs, paragraph);) {
        std::istringstream words(paragraph);
        std::string line;
        for (std::string word; words >> word;) {
            std::string candidate = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && ImGui::CalcTextSize(fa_shape(candidate).c_str()).x > width) {
                overlay_line(fa_shape(line).c_str(), true);
                line = std::move(word);
            } else {
                line = std::move(candidate);
            }
        }
        overlay_line(fa_shape(line).c_str(), true);
    }
}

static bool overlay_button(const char* label, bool rtl)
{
    if (rtl) align_overlay_item_right(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    return ImGui::Button(label);
}

static bool overlay_checkbox(const char* label, bool* value, bool rtl)
{
    if (!rtl) return ImGui::Checkbox(label, value);

    align_overlay_item_right(ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight()
        + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    const bool label_clicked = ImGui::IsItemClicked();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushID(label);
    const bool changed = ImGui::Checkbox("##toggle", value);
    ImGui::PopID();
    if (label_clicked) *value = !*value;
    return changed || label_clicked;
}

static std::string normalize_ip_field(const char* text)
{
    auto codepoints = PersianShaper::utf8_to_codepoints(text);
    for (auto &cp : codepoints) {
        if (cp >= 0x06F0 && cp <= 0x06F9) cp = '0' + cp - 0x06F0;
        if (cp >= 0x0660 && cp <= 0x0669) cp = '0' + cp - 0x0660;
    }
    return PersianShaper::codepoints_to_utf8(codepoints);
}

static bool overlay_peer_visible(const PeerDiscoverySnapshot& discovery, uint64 friend_id)
{
    return discovery.automatic_discovery
        || std::find(discovery.manual_users.begin(), discovery.manual_users.end(), CSteamID(friend_id)) != discovery.manual_users.end();
}


// ListBoxHeader() is deprecated and inlined inside <imgui.h>
// Helper to calculate size from items_count and height_in_items
static inline bool ImGuiHelper_BeginListBox(const char* label, int items_count) {
    int min_items = items_count < 7 ? items_count : 7;
    float height = ImGui::GetTextLineHeightWithSpacing() * (min_items + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f;
    return ImGui::BeginListBox(label, ImVec2(0.0f, height));
}


void Steam_Overlay::overlay_run_callback(void* object)
{
    // PRINT_DEBUG_ENTRY();
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->steam_run_callback();
}

void Steam_Overlay::overlay_networking_callback(void* object, Common_Message* msg)
{
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->networking_msg_received(msg);
}

void Steam_Overlay::parse_key_combo()
{
    static const std::unordered_map<InGameOverlay::ToggleKey, std::string_view> KEYS_MAP {
        { InGameOverlay::ToggleKey::SHIFT, "shift" },
        { InGameOverlay::ToggleKey::CTRL,  "ctrl"  },
        { InGameOverlay::ToggleKey::ALT,   "alt"   },
        { InGameOverlay::ToggleKey::TAB,   "tab"   },
        { InGameOverlay::ToggleKey::F1,    "fn1"   },
        { InGameOverlay::ToggleKey::F2,    "fn2"   },
        { InGameOverlay::ToggleKey::F3,    "fn3"   },
        { InGameOverlay::ToggleKey::F4,    "fn4"   },
        { InGameOverlay::ToggleKey::F5,    "fn5"   },
        { InGameOverlay::ToggleKey::F6,    "fn6"   },
        { InGameOverlay::ToggleKey::F7,    "fn7"   },
        { InGameOverlay::ToggleKey::F8,    "fn8"   },
        { InGameOverlay::ToggleKey::F9,    "fn9"   },
        { InGameOverlay::ToggleKey::F10,   "fn10"  },
        { InGameOverlay::ToggleKey::F11,   "fn11"  },
        { InGameOverlay::ToggleKey::F12,   "fn12"  },
    };

    std::unordered_set<InGameOverlay::ToggleKey> keys_combo{};
    bool use_default = false;
    if (settings->overlay_toggle_keys.empty()) {
        use_default = true;
    } else {
        for (const auto &key_name : settings->overlay_toggle_keys) {
            auto key_it = std::find_if(KEYS_MAP.cbegin(), KEYS_MAP.cend(), [&key_name](decltype(*KEYS_MAP.cbegin()) const &item) {
                return common_helpers::str_cmp_insensitive(item.second, key_name);
            });
            if (KEYS_MAP.cend() != key_it) {
                keys_combo.insert(key_it->first);
            } else {
                use_default = true;
                PRINT_DEBUG("[X] Unknown key '%s', using default key combo Shift + Tab", key_name.c_str());
                break;
            }
        }
    }

    if (use_default) {
        toggle_keys = {
            InGameOverlay::ToggleKey::SHIFT, InGameOverlay::ToggleKey::TAB
        };
    } else {
        toggle_keys = std::vector<InGameOverlay::ToggleKey>(keys_combo.begin(), keys_combo.end());
    }
}

void Steam_Overlay::parse_screenshot_key_combo()
{
    static const std::unordered_map<InGameOverlay::ToggleKey, std::string_view> KEYS_MAP {
        { InGameOverlay::ToggleKey::SHIFT, "shift" },
        { InGameOverlay::ToggleKey::CTRL,  "ctrl"  },
        { InGameOverlay::ToggleKey::ALT,   "alt"   },
        { InGameOverlay::ToggleKey::TAB,   "tab"   },
        { InGameOverlay::ToggleKey::F1,    "fn1"   },
        { InGameOverlay::ToggleKey::F2,    "fn2"   },
        { InGameOverlay::ToggleKey::F3,    "fn3"   },
        { InGameOverlay::ToggleKey::F4,    "fn4"   },
        { InGameOverlay::ToggleKey::F5,    "fn5"   },
        { InGameOverlay::ToggleKey::F6,    "fn6"   },
        { InGameOverlay::ToggleKey::F7,    "fn7"   },
        { InGameOverlay::ToggleKey::F8,    "fn8"   },
        { InGameOverlay::ToggleKey::F9,    "fn9"   },
        { InGameOverlay::ToggleKey::F10,   "fn10"  },
        { InGameOverlay::ToggleKey::F11,   "fn11"  },
        { InGameOverlay::ToggleKey::F12,   "fn12"  },
    };

    std::unordered_set<InGameOverlay::ToggleKey> keys_combo{};
    bool use_default = false;
    if (settings->overlay_screenshot_keys.empty()) {
        use_default = true;
    } else {
        for (const auto &key_name : settings->overlay_screenshot_keys) {
            auto key_it = std::find_if(KEYS_MAP.cbegin(), KEYS_MAP.cend(), [&key_name](decltype(*KEYS_MAP.cbegin()) const &item) {
                return common_helpers::str_cmp_insensitive(item.second, key_name);
            });
            if (KEYS_MAP.cend() != key_it) {
                keys_combo.insert(key_it->first);
            } else {
                use_default = true;
                PRINT_DEBUG("[X] Unknown screenshot key '%s', using default F12", key_name.c_str());
                break;
            }
        }
    }

    if (use_default) {
        screenshot_keys = {
            InGameOverlay::ToggleKey::F12
        };
    } else {
        screenshot_keys = std::vector<InGameOverlay::ToggleKey>(keys_combo.begin(), keys_combo.end());
    }
}

Steam_Overlay::Steam_Overlay(Settings* settings, Local_Storage *local_storage, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking* network, PlaytimeCounter* playtime_counter) :
    settings(settings),
    local_storage(local_storage),
    callback_results(callback_results),
    callbacks(callbacks),
    run_every_runcb(run_every_runcb),
    network(network),
    playtime_counter(playtime_counter),
    stats(Steam_Overlay_Stats(settings))
{
    // don't even bother initializing the overlay
    if (settings->disable_overlay) return;

    renderer_hook_init_thread = common_helpers::KillableWorker(
        [this](void *){ return renderer_hook_proc(); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(renderer_detector_polling_ms),
        [this] { return !setup_overlay_called; }
    );

    renderer_detector_delay_thread = common_helpers::KillableWorker(
        [this](void *){
            request_renderer_detector();
            renderer_hook_init_thread.start();
            return true;
        },
        std::chrono::milliseconds(settings->overlay_hook_delay_sec * 1000),
        std::chrono::milliseconds(0),
        [this] { return !setup_overlay_called; }
    );

    parse_key_combo();
    parse_screenshot_key_combo();
    strncpy(username_text, settings->get_local_name(), sizeof(username_text));
    displayed_local_name = settings->get_local_name();
    displayed_app_id = settings->get_local_game_id().AppID();

    // we need these copies to show the warning only once, then disable the flag
    // avoid manipulating settings->xxx
    this->warn_local_save =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_local_save && settings->overlay_warn_local_save;
    this->warn_bad_appid =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_bad_appid && settings->get_local_game_id().AppID() == 0;

    current_language = 0;
    const char *language = settings->get_overlay_language();

    show_user_info = settings->overlay_always_show_user_info;
    show_notification_history = settings->overlay_appearance.show_notification_history;
    show_achievements = settings->overlay_appearance.show_achievement_list;

    int i = 0;
    for (auto &lang : valid_languages) {
        if (common_helpers::str_cmp_insensitive(lang, language)) {
            current_language = i;
            break;
        }

        ++i;
    }

    this->network->setCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->add(&Steam_Overlay::overlay_run_callback, this);
}

Steam_Overlay::~Steam_Overlay()
{
    if (settings->disable_overlay) return;

    UnSetupOverlay();

    this->network->rmCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->remove(&Steam_Overlay::overlay_run_callback, this);
}

void Steam_Overlay::log_overlay_startup(const char *message) const
{
    try {
        const auto folder = std::filesystem::u8path(local_storage->get_path(""));
        if (folder.empty()) return;

        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error) return;

        std::ofstream log(folder / "overlay-startup.log", std::ios::app);
        log << '[' << common_helpers::get_utc_time() << " UTC] " << message << '\n';
    } catch (const std::exception &) {
        PRINT_DEBUG("could not write the overlay startup log");
    }
}

void Steam_Overlay::request_renderer_detector()
{
    PRINT_DEBUG_ENTRY();
    renderer_detector_active = true;
    InGameOverlay::DetectRenderer();
}

void Steam_Overlay::cleanup_renderer_hook()
{
    if (!renderer_detector_active) return;
    InGameOverlay::StopRendererDetection();
    InGameOverlay::FreeDetector();
    renderer_detector_active = false;
}

bool Steam_Overlay::renderer_hook_proc()
{
    InGameOverlay::RendererHook_t *detected = nullptr;
    const auto timeout = std::chrono::seconds(settings->overlay_renderer_detector_timeout_sec);
    for (int attempt = 1; attempt <= 2 && setup_overlay_called; ++attempt) {
        if (attempt == 2) request_renderer_detector();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (setup_overlay_called) {
            if (!InGameOverlay::DetectRenderer()) {
                detected = InGameOverlay::GetDetectedRenderer();
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        cleanup_renderer_hook();
        if (detected || !setup_overlay_called) break;
        PRINT_DEBUG("renderer detection timed out (attempt %i of 2)", attempt);
    }
    if (!detected) {
        if (setup_overlay_called) log_overlay_startup("Renderer detection did not find a renderer after two attempts.");
        return true;
    }

    {
        std::lock_guard lock(overlay_mutex);
        _renderer = detected;
    }

    // Steam shutdown can hold global_mutex while waiting for this worker.
    while (setup_overlay_called) {
        std::unique_lock client_lock(global_mutex, std::try_to_lock);
        if (client_lock.owns_lock()) {
            std::lock_guard lock(overlay_mutex);
            if (!setup_overlay_called) return true;
            load_achievements_data();
            get_steam_client()->steam_screenshots->overlay_take_screenshot = [this]() {
                std::lock_guard lock(overlay_mutex);
                if (Ready()) _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
            };
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!setup_overlay_called) return true;

    PRINT_DEBUG("got renderer hook %p for '%s'", _renderer, _renderer->GetLibraryName());

    // Load strings before building the font atlas so that all required glyphs are present.
    load_audio();
    create_fonts();
    if (!setup_overlay_called) return true;

    auto overlay_toggle_callback = [this]() { toggle_overlay_request(); };
    _renderer->OverlayProc = [this]() { overlay_render_proc(); };
    _renderer->OverlayHookReady = [this](InGameOverlay::OverlayHookState state) {
        PRINT_DEBUG("hook state changed to <%i>", (int)state);
        overlay_state_hook(state == InGameOverlay::OverlayHookState::Ready || state == InGameOverlay::OverlayHookState::Reset);
    };

    if (!_renderer->StartHook(overlay_toggle_callback, toggle_keys.data(), (int)toggle_keys.size(), &fonts_atlas)) {
        is_ready = false;
        log_overlay_startup("Renderer detected, but the overlay renderer hook failed to start.");
        PRINT_DEBUG("failed to start renderer hook");
        return true;
    }
    PRINT_DEBUG("started renderer hook");
    _renderer->SetScreenshotCallback(&Steam_Overlay::on_screenshot_captured, this);
    log_overlay_startup("Renderer hook installed.");
    renderer_hook_installed = true;

    return true;
}

// note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
void Steam_Overlay::create_fonts()
{
    PRINT_DEBUG_ENTRY();

    // disable rounding the texture height to the next power of two
    // see this: https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#4-font-atlas-texture-fails-to-upload-to-gpu
    fonts_atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    float font_size = settings->overlay_appearance.font_size;
    float font_size_fps = settings->overlay_appearance.font_size_fps > 0.0f
        ? settings->overlay_appearance.font_size_fps
        : font_size;
    float font_size_ach_title = settings->overlay_appearance.font_size_ach_title > 0.0f
        ? settings->overlay_appearance.font_size_ach_title
        : font_size;
    float font_size_ach_desc = settings->overlay_appearance.font_size_ach_desc > 0.0f
        ? settings->overlay_appearance.font_size_ach_desc
        : font_size;

    font_cfg.FontDataOwnedByAtlas = false; // https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#loading-font-data-from-memory
    font_cfg.PixelSnapH = true;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.SizePixels = font_size;
    // non-latin characters look ugly and squeezed without this horizontal spacing

    font_cfg.GlyphExtraAdvanceX = settings->overlay_appearance.font_glyph_extra_spacing_x;
    // font_cfg.GlyphExtraSpacing.x = settings->overlay_appearance.font_glyph_extra_spacing_x;
    // font_cfg.GlyphExtraSpacing.y = settings->overlay_appearance.font_glyph_extra_spacing_y;

    for (const auto &ach : achievements) {
        font_builder.AddText(ach.title.c_str());
        font_builder.AddText(ach.description.c_str());
    }
    for (int i = 0; i < TRANSLATION_NUMBER_OF_LANGUAGES; i++) {
        font_builder.AddText(translationChat[i]);
        font_builder.AddText(translationCopyId[i]);
        font_builder.AddText(translationTestAchievement[i]);
        font_builder.AddText(translationInvite[i]);
        font_builder.AddText(translationInviteAll[i]);
        font_builder.AddText(translationJoin[i]);
        font_builder.AddText(translationInvitedYouToJoinTheGame[i]);
        font_builder.AddText(translationAccept[i]);
        font_builder.AddText(translationRefuse[i]);
        font_builder.AddText(translationSend[i]);
        font_builder.AddText(translationUserPlaying[i]);
        font_builder.AddText(translationTotalTime[i]);
        font_builder.AddText(translationTotalTimeText[i]);
        font_builder.AddText(translationRenderer[i]);
        font_builder.AddText(translationShowAchievements[i]);
        font_builder.AddText(translationSettings[i]);
        font_builder.AddText(translationHistory[i]);
		font_builder.AddText(translationScreenshots[i]);
        font_builder.AddText(translationFriends[i]);
        font_builder.AddText(translationNoNotification[i]);
        font_builder.AddText(translationClearAll[i]);
        font_builder.AddText(translationHistoryChat[i]);
        font_builder.AddText(translationHistoryInvite[i]);
        font_builder.AddText(translationHistoryAchievement[i]);
        font_builder.AddText(translationHistoryProgress[i]);
        font_builder.AddText(translationHistoryAutoInvite[i]);
        font_builder.AddText(translationHistoryScreenshot[i]);
        font_builder.AddText(translationAchievementWindow[i]);
        font_builder.AddText(translationListOfAchievements[i]);
        font_builder.AddText(translationAchievements[i]);
        font_builder.AddText(translationHiddenAchievement[i]);
        font_builder.AddText(translationShow[i]);
        font_builder.AddText(translationAchievedOn[i]);
        font_builder.AddText(translationUnlocked[i]);
        font_builder.AddText(translationNoUnlockedAchievements[i]);
        font_builder.AddText(translationLocked[i]);
        font_builder.AddText(translationAllAchievementsUnlocked[i]);
        font_builder.AddText(translationNotAchieved[i]);
        font_builder.AddText(translationGlobalSettingsWindow[i]);
        font_builder.AddText(translationGlobalSettingsWindowDescription[i]);
        font_builder.AddText(translationUsername[i]);
        font_builder.AddText(translationLanguage[i]);
        font_builder.AddText(translationSelectedLanguage[i]);
        font_builder.AddText(translationRestartTheGameToApply[i]);
        font_builder.AddText(translationSave[i]);
        font_builder.AddText(translationWarning[i]);
        font_builder.AddText(translationWarningDescription_badAppid[i]);
        font_builder.AddText(translationWarningDescription_localSave[i]);
        font_builder.AddText(translationSteamOverlayURL[i]);
        font_builder.AddText(translationClose[i]);
        font_builder.AddText(translationPlaying[i]);
		font_builder.AddText(translationScreenshotSaved[i]);
		font_builder.AddText(translationUnpinAll[i]);
		font_builder.AddText(translationDeleteSelected[i]);
		font_builder.AddText(translationOpenFolder[i]);
		font_builder.AddText(translationNoScreenshotsYet[i]);
		font_builder.AddText(translationDelete[i]);
		font_builder.AddText(translationScreenshotPreview[i]);
		font_builder.AddText(translationPrev[i]);
		font_builder.AddText(translationPin[i]);
		font_builder.AddText(translationCrop[i]);
		font_builder.AddText(translationNext[i]);
		font_builder.AddText(translationDeleteThisScreenshot[i]);
		font_builder.AddText(translationDeleteAllScelectedScreenshots[i]);
		font_builder.AddText(translationYes[i]);
		font_builder.AddText(translationNo[i]);
		font_builder.AddText(translationConfirmDelete[i]);
		font_builder.AddText(translationConfirm[i]);
		font_builder.AddText(translationCancel[i]);
		font_builder.AddText(translationPinnedScreenshots[i]);
		font_builder.AddText(translationOpacity[i]);
        font_builder.AddText(translationAutoAcceptFriendInvite[i]);
        font_builder.AddText(translationFpsCheckbox[i]);
        font_builder.AddText(translationFpsDisplay[i]);
        font_builder.AddText(translationFrametimeCheckbox[i]);
        font_builder.AddText(translationFrametimeDisplay[i]);
        font_builder.AddText(translationFrametimeUnitDisplay[i]);
        font_builder.AddText(translationPlaytimeCheckbox[i]);
        font_builder.AddText(translationPlaytimeDisplay[i]);
    }
    font_builder.AddRanges(fonts_atlas.GetGlyphRangesDefault());

    static const ImWchar persian_glyph_ranges[] = {
        0x0600, 0x06FF, // حروف پایه فارسی و عربی
        0x200C, 0x200F, // نیم‌فاصله
        0xFB50, 0xFDFF, // فرم‌های چسبیده فارسی (پ، چ، ژ، گ، ک، ی)
        0xFE70, 0xFEFF, // فرم‌های چسبیده استاندارد (اول، وسط، آخر)
        0
    };
    font_builder.AddRanges(persian_glyph_ranges);

    font_builder.BuildRanges(&ranges);
    std::string default_font = settings->overlay_appearance.font_override;
#ifdef __WINDOWS__
    if (default_font.empty()) {
        wchar_t windows_dir[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windows_dir, static_cast<UINT>(std::size(windows_dir)));
        if (length > 0 && length < std::size(windows_dir)) {
            const auto system_font = std::filesystem::path(windows_dir) / "Fonts" / "segoeui.ttf";
            std::error_code error;
            if (std::filesystem::is_regular_file(system_font, error)) default_font = system_font.u8string();
        }
    }
#endif
    auto add_overlay_font = [this, &default_font](float size, const std::string &custom_font = "") {
        font_cfg.SizePixels = size;
        font_cfg.MergeMode = false;

        const std::string &font_path = custom_font.empty() ? default_font : custom_font;
        ImFont *font = nullptr;
        if (font_path.size()) {
            font = fonts_atlas.AddFontFromFileTTF(font_path.c_str(), size, &font_cfg);
            if (font) {
                font_cfg.MergeMode = true; // merge next font into the custom font
            }
        }

        // note: base85 compressed arrays caused a compiler heap allocation error, regular compression is more guaranteed
        ImFont *fallback_font = fonts_atlas.AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, size, &font_cfg);
        return font ? font : fallback_font;
    };

    font_notif = font_default = add_overlay_font(font_size);
    font_fps = add_overlay_font(font_size_fps);
    font_ach_title = add_overlay_font(font_size_ach_title, settings->overlay_appearance.font_override_ach_title);
    font_ach_desc = add_overlay_font(font_size_ach_desc, settings->overlay_appearance.font_override_ach_desc);
    stats.font = font_fps;

    bool res = fonts_atlas.IsBuilt();
    PRINT_DEBUG("isbuilt fonts atlas (result=%i)", (int)res);

    reset_LastError();
}

void Steam_Overlay::load_audio()
{
    PRINT_DEBUG_ENTRY();

    for (auto &kv : wav_files) {
        std::string file_path{};
        unsigned int file_size{};

        // try local location first, then try global location
        for (const auto &settings_path : { Local_Storage::get_game_settings_path(), local_storage->get_global_settings_path() }) {
            file_path = settings_path + Steam_Overlay::ACH_SOUNDS_FOLDER + PATH_SEPARATOR + kv.first;
            file_size = file_size_(file_path);
            if (file_size) break;
        }

        kv.second.clear();
        if (file_size) {
            kv.second.assign(file_size + 1, 0); // +1 because this will be treated as a null-terminated string later
            int read = Local_Storage::get_file_data(file_path, (char *)&kv.second[0], file_size);
            if (read <= 0) kv.second.clear();
            PRINT_DEBUG("loaded '%s' (read %i/%u bytes)", file_path.c_str(), read, file_size);
        }
    }
}

void Steam_Overlay::load_achievements_data()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats* steamUserStats = get_steam_client()->steam_user_stats;
    uint32 achievements_num = steamUserStats->GetNumAchievements();
    for (uint32 i = 0; i < achievements_num; ++i) {
        Overlay_Achievement ach{};
        ach.name = steamUserStats->GetAchievementName(i);
        ach.title = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "name");
        ach.description = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "desc");

        const char *hidden = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "hidden");
        ach.hidden = hidden && hidden[0] == '1';

        ach.unlock_percentage = static_cast<float>(
            steamUserStats->GetAchievementUnlockPercentage(ach.name.c_str()));

        bool achieved = false;
        uint32 unlock_time = 0;
        if (steamUserStats->GetAchievementAndUnlockTime(ach.name.c_str(), &achieved, &unlock_time)) {
            ach.achieved = achieved;
            ach.unlock_time = unlock_time;
        } else {
            ach.achieved = false;
            ach.unlock_time = 0;
        }

        float pnMinProgress = 0, pnMaxProgress = 0;
        if (steamUserStats->GetAchievementProgressLimits(ach.name.c_str(), &pnMinProgress, &pnMaxProgress)) {
            ach.progress = (uint32)pnMinProgress;
            ach.max_progress = (uint32)pnMaxProgress;
        }

        if (ach.icon == nullptr) {
            ach.icon = _renderer->CreateResource();
        }
        if (ach.icon_gray == nullptr) {
            ach.icon_gray = _renderer->CreateResource();
        }

        achievements.emplace_back(ach);

        if (!setup_overlay_called) return;
    }

    PRINT_DEBUG("count=%u, loaded=%zu", achievements_num, achievements.size());

}

// called initially and when window size is updated
void Steam_Overlay::overlay_state_hook(bool ready)
{
    PRINT_DEBUG("%i", (int)ready);
    if (!setup_overlay_called) return;

    if (!ready) late_init_imgui = false;
    if (ready && ImGui::GetCurrentContext()) ImGui::GetIO().IniFilename = nullptr;
    is_ready = ready;
}

// called when the user presses SHIFT + TAB
void Steam_Overlay::toggle_overlay_request()
{
    if (!setup_overlay_called) return;
    bool requested = overlay_requested.load();
    while (!overlay_requested.compare_exchange_weak(requested, !requested)) {}
}

void Steam_Overlay::allow_renderer_frame_processing(bool state, bool cleaning_up_overlay)
{
    // this is very important internally it calls the necessary fuctions
    // to properly update ImGui window size on the next overlay_render_proc() call

    if (state) {
        auto new_val = ++renderer_frame_processing_requests;
        if (new_val == 1) { // only take an action on first request
            // allow internal frmae processing
            _renderer->HideOverlayInputs(false);
            PRINT_DEBUG("enabled frame processing (count=%u)", new_val);
        }
    } else {
        if (renderer_frame_processing_requests > 0) {
            auto new_val = --renderer_frame_processing_requests;
            if (!new_val || cleaning_up_overlay) { // only take an action when the requests reach 0 or by force
                _renderer->HideOverlayInputs(true);
                PRINT_DEBUG("disabled frame processing (count=%u, force=%i)", new_val, (int)cleaning_up_overlay);
            }
        }
    }
}

void Steam_Overlay::obscure_game_input(bool state) {
    if (state) {
        auto new_val = ++obscure_cursor_requests;
        if (new_val == 1) { // only take an action on first request
            if (ImGui::GetCurrentContext()) {
                ImGuiIO &io = ImGui::GetIO();
                io.MouseDrawCursor = true;
                io.WantCaptureMouse = true;
                io.WantCaptureKeyboard = true;
            }
            _renderer->HideAppInputs(true);
            PRINT_DEBUG("obscured app input (count=%u)", new_val);
        }
    } else {
        if (obscure_cursor_requests > 0) {
            auto new_val = --obscure_cursor_requests;
            if (!new_val) { // only take an action when the requests reach 0
                if (ImGui::GetCurrentContext()) {
                    ImGuiIO &io = ImGui::GetIO();
                    io.MouseDrawCursor = false;
                    io.WantCaptureMouse = false;
                    io.WantCaptureKeyboard = false;
                }
                _renderer->HideAppInputs(false);
                PRINT_DEBUG("restored app input (count=%u)", new_val);
            }
        }
    }
}

void Steam_Overlay::notify_sound_user_invite(friend_window_state& friend_state)
{
    if (settings->disable_overlay_friend_notification) return;

    if (!(friend_state.window_state & window_state_show)) {
        friend_state.window_state |= window_state_need_attention;
#ifdef __WINDOWS__
        auto wav_data = wav_files.find("overlay_friend_notification.wav");
        if (wav_files.end() != wav_data && wav_data->second.size()) {
            PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
        } else {
            PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
        }
#endif
    }
}

void Steam_Overlay::notify_sound_user_achievement()
{
    if (settings->disable_overlay_achievement_notification) return;

#ifdef __WINDOWS__
    auto wav_data = wav_files.find("overlay_achievement_notification.wav");
    if (wav_files.end() != wav_data && wav_data->second.size()) {
        PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
    }
#endif
}

void Steam_Overlay::notify_sound_auto_accept_friend_invite()
{
#ifdef __WINDOWS__
    auto wav_data = wav_files.find("overlay_friend_notification.wav");
    if (wav_files.end() != wav_data && wav_data->second.size()) {
        PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
    } else {
        PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
    }
#endif
}

int find_free_id(std::vector<int> &ids, int base)
{
    std::sort(ids.begin(), ids.end());

    int id = base;
    for (auto i : ids)
    {
        if (id < i)
            break;
        id = i + 1;
    }

    return id > (base+max_window_id) ? 0 : id;
}

int find_free_friend_id(const std::map<Friend, friend_window_state, Friend_Less> &friend_windows)
{
    std::vector<int> ids{};
    ids.reserve(friend_windows.size());

    std::for_each(friend_windows.begin(), friend_windows.end(), [&ids](std::pair<Friend const, friend_window_state> const& i)
    {
        ids.emplace_back(i.second.id);
    });

    return find_free_id(ids, base_friend_window_id);
}

int find_free_notification_id(std::vector<Notification> const& notifications)
{
    std::vector<int> ids{};
    ids.reserve(notifications.size());

    std::for_each(notifications.begin(), notifications.end(), [&ids](Notification const& i)
    {
        ids.emplace_back(i.id);
    });


    return find_free_id(ids, base_friend_window_id);
}

bool Steam_Overlay::submit_notification(
    notification_type type,
    const std::string &msg,
    std::pair<const Friend, friend_window_state> *frd,
    Overlay_Achievement *ach)
{
    PRINT_DEBUG("%i", (int)type);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return false;

    int id = find_free_notification_id(notifications);
    if (id == 0) {
        PRINT_DEBUG("error no free id to create a notification window");
        return false;
    }

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)type;
    notif.message = msg;
    notif.frd = frd;
    if (ach) notif.ach = *ach;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    // uncomment this block to obscure cursor input and steal focus for these specific notifications
    switch (type) {
        // we want to steal focus for these ones
        case notification_type::invite:
            obscure_game_input(true);
        break;

        // not effective
        case notification_type::achievement_progress:
        case notification_type::achievement:
        case notification_type::auto_accept_invite:
        case notification_type::message:
        case notification_type::screenshot:
            // nothing
        break;

        default:
            PRINT_DEBUG("error unhandled type %i", (int)type);
        break;
    }

    return true;
}

void Steam_Overlay::add_chat_message_notification(std::string const &message)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG("'%s'", message.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    submit_notification(notification_type::message, message);
}

void Steam_Overlay::show_test_achievement()
{
    PRINT_DEBUG_ENTRY();
    Overlay_Achievement ach{};
    ach.title = tr_ui(current_language, translationTestAchievement, "تست دستاورد");
    ach.description = "~~~ " + ach.title + " ~~~";
    ach.achieved = true;

    // random add icon
    if (achievements.size()) {
        size_t rand_idx = common_helpers::rand_number(achievements.size() - 1);
        auto &rand_ach = achievements[rand_idx];
        bool achieved = rand_idx < (achievements.size() / 2);
        // force upload to GPU if the pagination is request-based
        try_load_ach_icon(rand_ach, achieved, settings->paginated_achievements_icons == 0);
        ach.icon = rand_ach.icon;
        ach.icon_gray = rand_ach.icon_gray;
    }

    // randomly add progress
    bool for_progress = false;
    if (common_helpers::rand_number(1000) % 4 == 0) {
        for_progress = true;
        uint32 progress = (uint32)(common_helpers::rand_number(500) / 10 + 50); // [50, 100]
        ach.max_progress = 100;
        ach.progress = progress;
        ach.achieved = false;
    } else if (common_helpers::rand_number(1000) % 2) {
        ach.unlock_percentage = 1.0f;
    }

    post_achievement_notification(ach, for_progress);
    // sound is now played when notification is actually shown (delayed with queue)
}

void Steam_Overlay::build_friend_context_menu(Friend const& frd, friend_window_state& state)
{
    if (ImGui::BeginPopupContextItem("Friends_ContextMenu", 1)) {
        // this is set to true if any button was clicked
        // otherwise, after clicking any button, the menu will be persistent
        bool close_popup = false;

        // user clicked on "chat"
        if (ImGui::Button(tr_ui(current_language, translationChat, "گفت‌وگو"))) {
            close_popup = true;
            state.window_state |= window_state_show;
        }
        // user clicked on "copy id" on a friend
        if (ImGui::Button(tr_ui(current_language, translationCopyId, "کپی آیدی استیم"))) {
            close_popup = true;
            auto friend_id_str = std::to_string(frd.id());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }
        // If we have the same appid, activate the invite/join buttons
        if (displayed_app_id == frd.appid()) {
            // user clicked on "invite to game"
            std::string translationInvite_tmp(tr_ui(current_language, translationInvite, "دعوت"));
            translationInvite_tmp.append("##PopupInviteToGame");
            if (i_have_lobby && ImGui::Button(translationInvite_tmp.c_str())) {
                close_popup = true;
                state.window_state |= window_state_invite;
                has_friend_action.push(frd);
            }

            // user clicked on "accept game invite"
            std::string translationJoin_tmp(tr_ui(current_language, translationJoin, "پیوستن"));
            translationJoin_tmp.append("##PopupAcceptInvite");
            if (state.joinable && ImGui::Button(translationJoin_tmp.c_str())) {
                close_popup = true;
                // don't bother adding this friend if the button "invite all" was clicked
                // we will send them the invitation later in Steam_Overlay::steam_run_callback()
                if (!invite_all_friends_clicked) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
            }
        }

        if (close_popup || invite_all_friends_clicked) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Steam_Overlay::build_friend_window(Friend const& frd, friend_window_state& state)
{
    if (!(state.window_state & window_state_show))
        return;

    bool show = true;
    bool send_chat_msg = false;

    float width = ImGui::CalcTextSize("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA").x;

    ImGui::SetNextWindowSizeConstraints(ImVec2{ width, ImGui::GetFontSize()*8 + ImGui::GetFrameHeightWithSpacing()*4 },
        ImVec2{ std::numeric_limits<float>::max() , std::numeric_limits<float>::max() });

    ImGui::SetNextWindowBgAlpha(1.0f);
    // Window id is after the ###, the window title is the friend name
    std::string friend_window_id = std::move("###" + std::to_string(state.id));
    if (ImGui::Begin((state.window_title + friend_window_id).c_str(), &show)) {
        if (state.window_state & window_state_need_attention && ImGui::IsWindowFocused()) {
            state.window_state &= ~window_state_need_attention;
        }

        // Fill this with the chat box and maybe the invitation
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            ImGui::LabelText("##label", translationInvitedYouToJoinTheGame[current_language], frd.name().c_str(), frd.appid());
            ImGui::SameLine();
            if (ImGui::Button(translationAccept[current_language])) {
                state.window_state |= window_state_join;
                this->has_friend_action.push(frd);
            }

            ImGui::SameLine();
            if (ImGui::Button(translationRefuse[current_language])) {
                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
            }
        }

        ImGui::InputTextMultiline("##chat_history", state.chat_history.data(), state.chat_history.size() + 1,
            { -1.0f, -ImGui::GetFrameHeightWithSpacing() }, ImGuiInputTextFlags_ReadOnly);
        const char* send_label = tr_ui(current_language, translationSend, "ارسال");
        const auto &style = ImGui::GetStyle();
        const float send_width = ImGui::CalcTextSize(send_label).x + style.FramePadding.x * 2;
        const float input_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - send_width - style.ItemSpacing.x);

        uint64_t frd_id = frd.id();
        ImGui::PushID((const char *)&frd_id, (const char *)&frd_id + sizeof(frd_id));
        ImGui::PushItemWidth(input_width);

        if (ImGui::InputText("##chat_line", state.chat_input, max_chat_len, ImGuiInputTextFlags_EnterReturnsTrue)) {
            send_chat_msg = true;
            ImGui::SetKeyboardFocusHere(-1);
        }

        ImGui::PopItemWidth();
        ImGui::PopID();

        ImGui::SameLine();

        if (ImGui::Button(send_label)) {
            send_chat_msg = true;
        }

        if (send_chat_msg) {
            if (!(state.window_state & window_state_send_message)) {
                has_friend_action.push(frd);
                state.window_state |= window_state_send_message;
            }
        }
    }

    // User closed the friend window
    if (!show) {
        state.window_state &= ~window_state_show;
    }

    ImGui::End();
}


std::chrono::milliseconds Steam_Overlay::get_notification_duration(notification_type type)
{
    switch (type)
    {
    case notification_type::message:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_chat);

    case notification_type::invite:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);

    case notification_type::achievement:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_achievement);

    case notification_type::achievement_progress:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_progress);

    case notification_type::auto_accept_invite:
        return Notification::default_show_time;

    case notification_type::screenshot:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_screenshot);
    }

    PRINT_DEBUG("ERROR unhandled type %i", (int)type);
    return Notification::default_show_time;
}

// set the position of the next notification
void Steam_Overlay::set_next_notification_pos(std::pair<float, float> scrn_size, std::chrono::milliseconds elapsed, std::chrono::milliseconds duration, const Notification &noti, struct NotificationsCoords &coords)
{
    const float scrn_width = scrn_size.first;
    const float scrn_height = scrn_size.second;

    auto &global_style = ImGui::GetStyle();
    const float padding_all_sides = 2 * (global_style.WindowPadding.y + global_style.WindowPadding.x);

    const float noti_width = scrn_width * Notification::width_percent;
    const float msg_height = ImGui::CalcTextSize(
        noti.message.c_str(),
        noti.message.c_str() + noti.message.size(),
        false,
        noti_width - padding_all_sides - global_style.ItemSpacing.x
    ).y;
    float noti_height = msg_height;

    // get the required position
    Overlay_Appearance::NotificationPosition pos = Overlay_Appearance::default_pos;
    switch ((notification_type)noti.type) {
    case notification_type::achievement_progress:
    case notification_type::achievement: {
        pos = settings->overlay_appearance.ach_earned_pos;

        const auto &ach = noti.ach.value();
        const float ach_text_width = noti_width - padding_all_sides - global_style.ItemSpacing.x - settings->overlay_appearance.icon_size;
        ImGui::PushFont(font_ach_title);
        float new_msg_height = ImGui::CalcTextSize(
            ach.title.c_str(),
            ach.title.c_str() + ach.title.size(),
            false,
            ach_text_width
        ).y;
        ImGui::PopFont();
        if (ach.description.size()) {
            ImGui::PushFont(font_ach_desc);
            new_msg_height += global_style.ItemSpacing.y + ImGui::CalcTextSize(
                ach.description.c_str(),
                ach.description.c_str() + ach.description.size(),
                false,
                ach_text_width
            ).y;
            ImGui::PopFont();
        }
        const float new_noti_height = new_msg_height;

        float biggest_noti_height = settings->overlay_appearance.icon_size;
        if (biggest_noti_height < new_noti_height) biggest_noti_height = new_noti_height;

        // Account for table cell padding: the row needs (icon/text height) + 2*CP,
        // but the window border clips the bottom CP.  3*CP covers both cell
        // padding directions AND the border inset so the spacing is symmetric.
        noti_height = biggest_noti_height + 3.0f * global_style.CellPadding.y;

        if ((notification_type)noti.type == notification_type::achievement_progress) {
            if (!noti.ach.value().achieved && noti.ach.value().max_progress > 0) {
                noti_height += settings->overlay_appearance.font_size + global_style.WindowPadding.y;
            }
        }
    }
    break;

    // case notification_type::invite: pos = settings->overlay_appearance.invite_pos; break;
    case notification_type::invite: {
        pos = settings->overlay_appearance.invite_pos;
        const float msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::message: pos = settings->overlay_appearance.chat_msg_pos; break;
    default: PRINT_DEBUG("ERROR: unhandled notification type %i", (int)noti.type); break;
    }
    // add some y padding for niceness
    noti_height += 2 * global_style.WindowPadding.y;

    // 0 on the y-axis is top, 0 on the x-axis is left
    float x = 0.0f;
    float y = 0.0f;
    float animate_size = 0.0f;
    const float margin_y = settings->overlay_appearance.notification_margin_y;
    const float margin_x = settings->overlay_appearance.notification_margin_x;

    switch (pos) {
    // top
    case Overlay_Appearance::NotificationPosition::top_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = coords.top_left.second + margin_y;
        coords.top_left.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = coords.top_center.second + margin_y - animate_size;
        coords.top_center.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = coords.top_right.second + margin_y;
        coords.top_right.second = y + noti_height;
    break;

    // bot
    case Overlay_Appearance::NotificationPosition::bot_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = scrn_height - coords.bot_left.second - margin_y - noti_height;
        coords.bot_left.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = scrn_height - coords.bot_center.second - margin_y - noti_height + animate_size;
        coords.bot_center.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = scrn_height - coords.bot_right.second - margin_y - noti_height;
        coords.bot_right.second = scrn_height - y;
    break;

    default: /* satisfy compiler warning */ break;
    }

    ImGui::SetNextWindowPos(ImVec2( x, y ));
    ImGui::SetNextWindowSize(ImVec2(noti_width, noti_height));
}

float Steam_Overlay::animate_factor(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration)
{
    if (settings->overlay_appearance.notification_animation <= 0) return 0.0f; // no animation

    std::chrono::milliseconds animation_duration(settings->overlay_appearance.notification_animation);
    // PRINT_DEBUG("ELAPSED %u/%u/%u", (uint32)elapsed.count(), (uint32)duration.count(), (uint32)animation_duration.count());

    float factor = 0.0f;
    if (elapsed < animation_duration) { // sliding in
        factor = 1.0f - (static_cast<float>(elapsed.count()) / animation_duration.count());
        // PRINT_DEBUG("SHOW FACTOR %f", factor);
    } else {
        // time between sliding in/out animation
        // here we add the animation duration because we want to count after the animation
        // if we have 1 sec animation & 2 sec show time:
        //   the duration will start at < 1 sec during the initial animation
        //   after the animation (1 sec), the duration will be >= 1 sec
        //   but since we want 2 sec show time, the duration must last 3 sec
        auto steady_time = animation_duration + duration;
        if (elapsed > steady_time) {
            factor = static_cast<float>((elapsed - steady_time).count()) / animation_duration.count();
            // PRINT_DEBUG("HIDE FACTOR %f", factor);
        }
    }

    return factor;
}

void Steam_Overlay::add_ach_progressbar(const Overlay_Achievement &ach)
{
    if (!ach.achieved && ach.max_progress > 0) {
        char buf[32]{};
        sprintf(buf, "%u/%u", ach.progress, ach.max_progress);
        ImGui::ProgressBar((float)ach.progress / ach.max_progress, { -1 , settings->overlay_appearance.font_size }, buf);
    }
}

ImVec4 Steam_Overlay::get_notification_bg_rgba_safe()
{
    if (settings->overlay_appearance.notification_r >= 0 &&
        settings->overlay_appearance.notification_g >= 0 &&
        settings->overlay_appearance.notification_b >= 0 &&
        settings->overlay_appearance.notification_a >= 0)
    {
        return ImVec4(
            settings->overlay_appearance.notification_r,
            settings->overlay_appearance.notification_g,
            settings->overlay_appearance.notification_b,
            settings->overlay_appearance.notification_a
        );
    }

    // fallback to dark-gray background
    return ImVec4(
        0.12f,
        0.14f,
        0.21f,
        1.0f
    );
}

void Steam_Overlay::build_notifications(float width, float height)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    std::queue<Friend> friend_actions_temp{};

    ImGui::PushFont(font_notif);
    // Add window rounding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);

    NotificationsCoords coords{};
    for (auto it = notifications.begin(); it != notifications.end(); ++it) {
        auto noti_duration = get_notification_duration((notification_type)it->type);
        if (noti_duration.count() <= 0) {
            it->expired = true;
            continue;
        }

        // *2 for sliding in & out animation
        auto total_allowed_duration = noti_duration + std::chrono::milliseconds(settings->overlay_appearance.notification_animation * 2);
        auto elapsed_notif = now - it->start_time;
        if (elapsed_notif > total_allowed_duration) {
            it->expired = true;
            continue;
        }

        float settings_noti_alpha = settings->overlay_appearance.notification_a >= 0.0f && settings->overlay_appearance.notification_a <= 1.0f
            ? settings->overlay_appearance.notification_a
            : 1.0f;

        const bool is_rare_achievement =
            (notification_type)it->type == notification_type::achievement &&
            it->ach.has_value() &&
            it->ach->unlock_percentage >= 0.0f &&
            it->ach->unlock_percentage <= 10.0f;
        const bool is_achievement =
            (notification_type)it->type == notification_type::achievement;

        ImGui::PushStyleColor(ImGuiCol_Border, is_rare_achievement
            ? ImVec4(32.0f / 255.0f, 24.0f / 255.0f, 8.0f / 255.0f, settings_noti_alpha)
            : ImVec4(0, 0, 0, settings_noti_alpha));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, get_notification_bg_rgba_safe());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 255, 255, settings_noti_alpha * 2));
        if (is_rare_achievement) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        }

        // some extra window flags for each notification type
        ImGuiWindowFlags extra_flags = ImGuiWindowFlags_NoFocusOnAppearing;
        switch ((notification_type)it->type) {
            // games like "Mafia Definitive Edition" will pause the entire game/scene if focus was stolen
            // be less intrusive for notifications that do not require interaction
            case notification_type::achievement_progress:
            case notification_type::achievement:
            case notification_type::auto_accept_invite:
            case notification_type::message:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::invite:
            case notification_type::screenshot:
                // nothing (needs input for buttons)
            break;

            default:
                PRINT_DEBUG("error unhandled flags for type %i", (int)it->type);
            break;
        }

        std::string wnd_name = "NotiPopupShow" + std::to_string(it->id);

        set_next_notification_pos({width, height}, elapsed_notif, noti_duration, *it, coords);
        if (ImGui::Begin(wnd_name.c_str(), nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | extra_flags)) {
            switch ((notification_type)it->type) {
                case notification_type::achievement_progress:
                case notification_type::achievement: {
                    const auto &ach = it->ach.value();
                    auto &icon_rsrc = (notification_type)it->type == notification_type::achievement
                        ? ach.icon
                        : ach.icon_gray;
                    if (icon_rsrc->GetResourceId() != 0 && ImGui::BeginTable("imgui_table", 2)) {
                        ImGui::TableSetupColumn("imgui_table_image", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                        ImGui::TableSetupColumn("imgui_table_text");
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushFont(font_ach_title);
                        ImGui::TextWrapped("%s", ach.title.c_str());
                        ImGui::PopFont();
                        if (ach.description.size()) {
                            ImGui::PushFont(font_ach_desc);
                            ImGui::TextWrapped("%s", ach.description.c_str());
                            ImGui::PopFont();
                        }

                        ImGui::EndTable();
                    } else {
                        ImGui::PushFont(font_ach_title);
                        ImGui::TextWrapped("%s", ach.title.c_str());
                        ImGui::PopFont();
                        if (ach.description.size()) {
                            ImGui::PushFont(font_ach_desc);
                            ImGui::TextWrapped("%s", ach.description.c_str());
                            ImGui::PopFont();
                        }
                    }

                    if ((notification_type)it->type == notification_type::achievement_progress) {
                        add_ach_progressbar(ach);
                    }
                }
                break;

                case notification_type::invite: {
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationJoin[current_language])) {
                        it->frd->second.window_state |= window_state_join;
                        friend_actions_temp.push(it->frd->first);
                        // when we click "accept game invite" from someone else, we want to remove this notification immediately since it's no longer relevant
                        // this assignment will make the notification elapsed time insanely large
                        it->start_time = {};
                    }
                }
                break;

                case notification_type::message:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::auto_accept_invite:
                case notification_type::screenshot:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                default:
                    PRINT_DEBUG("error unhandled notification for type %i", (int)it->type);
                break;
            }

            if (is_achievement) {
                const ImVec2 wnd_pos = ImGui::GetWindowPos();
                const ImVec2 wnd_size = ImGui::GetWindowSize();
                if (is_rare_achievement) {
                    // Glowing gold border for the notification window (same effect as the
                    // achievement list icon): 3 semi-transparent outer rings + a solid
                    // bright inner line.  Outer rings go in the background draw list so
                    // they can extend past the window bounds.
                    const float inset = 2.0f;
                    const ImVec2 border_min(wnd_pos.x + inset, wnd_pos.y + inset);
                    const ImVec2 border_max(wnd_pos.x + wnd_size.x - inset, wnd_pos.y + wnd_size.y - inset);
                    const float rounding = settings->overlay_appearance.notification_rounding;
                    const int   steps = 4;
                    const float step_px = 1.8f;
                    const float max_expand = step_px * (float)steps;

                    // Use the foreground draw list with a clip rect that extends past
                    // the window bounds, otherwise the window background clips the rings.
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->PushClipRect(
                        ImVec2(wnd_pos.x - max_expand, wnd_pos.y - max_expand),
                        ImVec2(wnd_pos.x + wnd_size.x + max_expand,
                               wnd_pos.y + wnd_size.y + max_expand),
                        false);
                    for (int i = steps; i >= 1; --i) {
                        const float expand = step_px * (float)i;
                        const ImVec2 lo(border_min.x - expand, border_min.y - expand);
                        const ImVec2 hi(border_max.x + expand, border_max.y + expand);
                        const int a = (int)(180.0f * (1.0f - (float)(i - 1) / (float)steps) * settings_noti_alpha);
                        dl->AddRect(lo, hi, IM_COL32(218, 165, 32, a), rounding + expand, 0, 2.0f);
                    }
                    dl->PopClipRect();

                    ImGui::GetWindowDrawList()->AddRect(
                        border_min,
                        border_max,
                        IM_COL32(255, 215, 90, (int)(220.0f * settings_noti_alpha)),
                        rounding,
                        0,
                        2.0f);
                } else {
                    const float inset = 1.0f;
                    const ImVec2 inner_min(wnd_pos.x + inset, wnd_pos.y + inset);
                    const ImVec2 inner_max(wnd_pos.x + wnd_size.x - inset, wnd_pos.y + wnd_size.y - inset);
                    const ImU32 accent_color = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(1.0f, 1.0f, 1.0f, 0.82f * settings_noti_alpha));

                    ImGui::GetWindowDrawList()->AddRect(
                        inner_min,
                        inner_max,
                        accent_color,
                        settings->overlay_appearance.notification_rounding,
                        0,
                        2.0f);
                }
            }

            if (is_rare_achievement) {
                const float shimmer_duration_ms = 700.0f;
                const float shimmer_delay_ms = 450.0f;
                const float shimmer_elapsed_ms = static_cast<float>(
                    elapsed_notif.count() - settings->overlay_appearance.notification_animation) - shimmer_delay_ms;
                if (shimmer_elapsed_ms >= 0.0f && shimmer_elapsed_ms <= shimmer_duration_ms) {
                    float shimmer_t = shimmer_elapsed_ms / shimmer_duration_ms;
                    if (shimmer_t < 0.0f) shimmer_t = 0.0f;
                    if (shimmer_t > 1.0f) shimmer_t = 1.0f;

                    const ImVec2 wnd_pos = ImGui::GetWindowPos();
                    const ImVec2 wnd_size = ImGui::GetWindowSize();
                    const ImVec2 wnd_max(wnd_pos.x + wnd_size.x, wnd_pos.y + wnd_size.y);
                    const float sweep_width = wnd_size.x * 0.18f;
                    const float sweep_x = wnd_pos.x - sweep_width + (wnd_size.x + sweep_width * 2.0f) * shimmer_t;
                    const float alpha = (1.0f - shimmer_t) * 0.22f * settings_noti_alpha;

                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    draw_list->PushClipRect(wnd_pos, wnd_max, true);
                    draw_list->AddQuadFilled(
                        ImVec2(sweep_x - sweep_width, wnd_pos.y),
                        ImVec2(sweep_x, wnd_pos.y),
                        ImVec2(sweep_x + sweep_width, wnd_max.y),
                        ImVec2(sweep_x, wnd_max.y),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.88f, 0.45f, alpha)));
                    draw_list->PopClipRect();
                }
            }

        }

        ImGui::End();

        if (is_rare_achievement) {
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();

    // erase all notifications whose visible time exceeded the max
    notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [this](const Notification &item) {
        if (item.expired) {
            PRINT_DEBUG("removing a notification");
            allow_renderer_frame_processing(false);
            // uncomment this block to restore app input focus
            switch ((notification_type)item.type) {
                // we want to restore focus for these ones
                case notification_type::invite:
                    obscure_game_input(false);
                break;

                // not effective
                case notification_type::achievement_progress:
                case notification_type::achievement:
                case notification_type::auto_accept_invite:
                case notification_type::message:
                case notification_type::screenshot:
                    // nothing
                break;

                default:
                    PRINT_DEBUG("error unhandled remove for type %i", (int)item.type);
                break;
            }

            // Archive to notification history (lightweight copy, no pointers/GPU resources)
            {
                NotificationHistoryEntry entry{};
                // Use actual achievement unlock time when available,
                // otherwise fall back to the notification display time.
                if (item.ach.has_value() && item.ach->unlock_time > 0) {
                    entry.timestamp = std::chrono::milliseconds(
                        static_cast<long long>(item.ach->unlock_time) * 1000);
                } else {
                    entry.timestamp = item.start_time;
                }
                entry.type = item.type;
                entry.message = item.message;
                if (notification_history.size() >= MAX_NOTIFICATION_HISTORY) {
                    notification_history.pop_front();
                }
                notification_history.push_back(std::move(entry));
                notification_history_cache_dirty = true;
            }

            return true;
        }

        return false;
    }), notifications.end());

    if (!friend_actions_temp.empty()) {
        while (!friend_actions_temp.empty()) {
            has_friend_action.push(friend_actions_temp.front());
            friend_actions_temp.pop();
        }
    }
}

void Steam_Overlay::add_auto_accept_invite_notification()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), "%s", translationAutoAcceptFriendInvite[current_language]);

    submit_notification(notification_type::auto_accept_invite, tmp);
    notify_sound_auto_accept_friend_invite();
}

void Steam_Overlay::add_invite_notification(std::pair<const Friend, friend_window_state>& wnd_state)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    char tmp[TRANSLATION_BUFFER_SIZE]{};
    auto &first_friend = wnd_state.first;
    auto &name = first_friend.name();
    snprintf(tmp, sizeof(tmp), translationInvitedYouToJoinTheGame[current_language], name.c_str(), (uint64)first_friend.appid());

    submit_notification(notification_type::invite, tmp, &wnd_state);
}

void Steam_Overlay::post_achievement_notification(Overlay_Achievement &ach, bool for_progress)
{
    if (settings->disable_overlay_achievement_notification) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
// Get current time
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

    // Calculate scheduled show time based on rate limiting
    std::chrono::milliseconds scheduled_show_time;
    int delay_ms = settings->achievement_notification_delay_ms;

    PRINT_DEBUG("Achievement delay: %d ms", delay_ms);

    if (delay_ms <= 0) {
        // No delay - show immediately
        scheduled_show_time = now;
    } else {
        // Apply rate limiting: earliest show time is last_scheduled_show_time + delay
        scheduled_show_time = std::max(now, last_scheduled_show_time + std::chrono::milliseconds(delay_ms));
    }

    // Create scheduled achievement entry
    ScheduledAchievement scheduled_ach;
    scheduled_ach.ach = ach;
    scheduled_ach.for_progress = for_progress;
    scheduled_ach.trigger_time = now;
    scheduled_ach.scheduled_show_time = scheduled_show_time;

    // Add to queue
    achievement_queue.push_back(scheduled_ach);

    // Update last scheduled show time for next item
    last_scheduled_show_time = scheduled_show_time;

    PRINT_DEBUG("Achievement queued: '%s', scheduled for %lld ms, delay=%d ms", 
                ach.name.c_str(), (long long)scheduled_show_time.count(), delay_ms);
}

void Steam_Overlay::process_achievement_queue()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    if (achievement_queue.empty()) return;

    // Get current time
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

    // Process all ready achievements
    while (!achievement_queue.empty()) {
        auto& scheduled_ach = achievement_queue.front();

        // Check if it's time to show this notification
        PRINT_DEBUG("Queue check: scheduled=%lld, now=%lld, diff=%lld",
                    (long long)scheduled_ach.scheduled_show_time.count(),
                    (long long)now.count(),
                    (long long)(now - scheduled_ach.scheduled_show_time).count());
        if (scheduled_ach.scheduled_show_time <= now) {
            // Show the notification
            bool achieved = !scheduled_ach.for_progress;
            // force upload to GPU if the pagination is request-based
            try_load_ach_icon(scheduled_ach.ach, achieved, settings->paginated_achievements_icons == 0);

            submit_notification(
                scheduled_ach.for_progress ? notification_type::achievement_progress : notification_type::achievement,
                scheduled_ach.ach.title + "\n" + scheduled_ach.ach.description,
                {},
                &scheduled_ach.ach
            );

            // Play sound when notification is actually shown (delayed with queue)
            notify_sound_user_achievement();

            PRINT_DEBUG("Achievement shown: '%s' at %lld ms", 
                        scheduled_ach.ach.name.c_str(), (long long)now.count());

            // Remove from queue
            achievement_queue.pop_front();
        } else {
            // This achievement is not ready yet, and queue is ordered by scheduled time,
            // so no more achievements will be ready either
            break;
        }
    }

}

bool Steam_Overlay::try_load_ach_icon(Overlay_Achievement &ach, bool achieved, bool upload_new_icon_to_gpu)
{
    if (!_renderer) return false;
    if (settings->paginated_achievements_icons < 0) return false; // no icons are loaded anyway
    if (!settings->overlay_upload_achs_icons_to_gpu) return false; // don't upload anything to the GPU

    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    if (icon_rsrc->GetResourceId() != 0) return true;

    // icons needs to be loaded, but we're not allowed
    if (!upload_new_icon_to_gpu) return false;

    // Steam callbacks can already hold global_mutex while waiting for overlay state.
    // Loading an icon is optional for this frame, so never wait for that lock here.
    std::unique_lock client_lock(global_mutex, std::try_to_lock);
    if (!client_lock) return false;

    int &icon_handle = achieved ? ach.icon_handle : ach.icon_gray_handle;
    if (Settings::UNLOADED_IMAGE_HANDLE == icon_handle) { // not loaded yet
        icon_handle = get_steam_client()->steam_user_stats->get_achievement_icon_handle(ach.name, achieved);
    }
    auto image_info = settings->get_image(icon_handle);
    if (image_info) {
        int icon_size = static_cast<int>(settings->overlay_appearance.icon_size);
        //icon_rsrc->SetAutoLoad(InGameOverlay::ResourceAutoLoad_t::OnUse);
        icon_rsrc->AttachResource((void*)image_info->data.c_str(), icon_size, icon_size);

        PRINT_DEBUG("'%s' (result=%i)", ach.name.c_str(), (int)icon_rsrc->GetResourceId() != 0);
    }

    return icon_rsrc->GetResourceId() != 0;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::overlay_render_proc()
{
    if (!setup_overlay_called || !renderer_hook_installed || !ImGui::GetCurrentContext()) return;

    // Keep drawing while the game is running Steam callbacks. The UI reads a cached
    // profile and uses its own synchronization for changes that reach the game.
    std::unique_lock lock(overlay_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !setup_overlay_called) return;

    if (!late_init_imgui.exchange(true)) {
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.MouseDrawCursor = show_overlay;
        _renderer->HideOverlayInputs(renderer_frame_processing_requests == 0);
        _renderer->HideAppInputs(obscure_cursor_requests > 0);
    }
    is_ready = true;

    const bool requested = overlay_requested;
    if (show_overlay != requested) {
        show_overlay = requested;
        overlay_state_changed = true;
        allow_renderer_frame_processing(requested);
        obscure_game_input(requested);
    }

    // Process achievement queue to show scheduled notifications
    process_achievement_queue();

    // -- Screenshot hotkey detection --
    if (_renderer && settings->enable_screenshot && !screenshot_keys.empty()) {
#ifdef __WINDOWS__
        // Only respond when the game window is focused
        bool screenshot_focused = true;
        {
            HWND fg = GetForegroundWindow();
            if (fg) {
                DWORD fg_pid = 0;
                GetWindowThreadProcessId(fg, &fg_pid);
                if (fg_pid != GetCurrentProcessId())
                    screenshot_focused = false;
            }
        }

        if (screenshot_focused) {
            // Map ToggleKey to Windows VK codes and check state
            auto toggleKeyToVK = [](InGameOverlay::ToggleKey key) -> int {
                switch (key) {
                    case InGameOverlay::ToggleKey::SHIFT: return VK_SHIFT;
                    case InGameOverlay::ToggleKey::CTRL:  return VK_CONTROL;
                    case InGameOverlay::ToggleKey::ALT:   return VK_MENU;
                    case InGameOverlay::ToggleKey::TAB:   return VK_TAB;
                    case InGameOverlay::ToggleKey::F1:    return VK_F1;
                    case InGameOverlay::ToggleKey::F2:    return VK_F2;
                    case InGameOverlay::ToggleKey::F3:    return VK_F3;
                    case InGameOverlay::ToggleKey::F4:    return VK_F4;
                    case InGameOverlay::ToggleKey::F5:    return VK_F5;
                    case InGameOverlay::ToggleKey::F6:    return VK_F6;
                    case InGameOverlay::ToggleKey::F7:    return VK_F7;
                    case InGameOverlay::ToggleKey::F8:    return VK_F8;
                    case InGameOverlay::ToggleKey::F9:    return VK_F9;
                    case InGameOverlay::ToggleKey::F10:   return VK_F10;
                    case InGameOverlay::ToggleKey::F11:   return VK_F11;
                    case InGameOverlay::ToggleKey::F12:   return VK_F12;
                    default: return 0;
                }
            };

            bool all_pressed = true;
            for (auto k : screenshot_keys) {
                int vk = toggleKeyToVK(k);
                if (!vk || !(GetAsyncKeyState(vk) & 0x8000)) {
                    all_pressed = false;
                    break;
                }
            }

            // Rising edge detection + cooldown (1 second).
            // `prev_initialized` ensures we don't fire a false trigger on the first call when the
            // user happens to be holding the hotkey when the overlay is first loaded.
            static bool prev_screenshot_keys = false;
            static bool prev_initialized = false;
            static std::chrono::steady_clock::time_point last_screenshot_time_local{};
            auto now_local = std::chrono::steady_clock::now();
            bool rising_edge = false;
            if (prev_initialized) {
                rising_edge = all_pressed && !prev_screenshot_keys;
            }
            prev_screenshot_keys = all_pressed;
            prev_initialized = true;

            if (rising_edge && (now_local - last_screenshot_time_local) > std::chrono::seconds(1)) {
                last_screenshot_time_local = now_local;
                PRINT_DEBUG("Screenshot hotkey triggered");
                _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
            }
        }
#else
        // Non-Windows: use ToggleKey to ImGui mapping
        auto toggleKeyToImGui = [](InGameOverlay::ToggleKey key) -> ImGuiKey {
            switch (key) {
                case InGameOverlay::ToggleKey::SHIFT: return ImGuiKey_LeftShift;
                case InGameOverlay::ToggleKey::CTRL:  return ImGuiKey_LeftCtrl;
                case InGameOverlay::ToggleKey::ALT:   return ImGuiKey_LeftAlt;
                case InGameOverlay::ToggleKey::TAB:   return ImGuiKey_Tab;
                case InGameOverlay::ToggleKey::F1:    return ImGuiKey_F1;
                case InGameOverlay::ToggleKey::F2:    return ImGuiKey_F2;
                case InGameOverlay::ToggleKey::F3:    return ImGuiKey_F3;
                case InGameOverlay::ToggleKey::F4:    return ImGuiKey_F4;
                case InGameOverlay::ToggleKey::F5:    return ImGuiKey_F5;
                case InGameOverlay::ToggleKey::F6:    return ImGuiKey_F6;
                case InGameOverlay::ToggleKey::F7:    return ImGuiKey_F7;
                case InGameOverlay::ToggleKey::F8:    return ImGuiKey_F8;
                case InGameOverlay::ToggleKey::F9:    return ImGuiKey_F9;
                case InGameOverlay::ToggleKey::F10:   return ImGuiKey_F10;
                case InGameOverlay::ToggleKey::F11:   return ImGuiKey_F11;
                case InGameOverlay::ToggleKey::F12:   return ImGuiKey_F12;
                default: return ImGuiKey_None;
            }
        };

        bool all_pressed = true;
        for (auto k : screenshot_keys) {
            ImGuiKey ik = toggleKeyToImGui(k);
            if (ik == ImGuiKey_None || !ImGui::IsKeyDown(ik)) {
                all_pressed = false;
                break;
            }
        }

        // Rising edge detection + cooldown (1 second). Skip the first frame so we don't
        // trigger a false-positive if the user is already holding the hotkey.
        static bool prev_screenshot_keys = false;
        static bool prev_initialized = false;
        static std::chrono::steady_clock::time_point last_screenshot_time_local{};
        auto now_local = std::chrono::steady_clock::now();
        bool rising_edge = false;
        if (prev_initialized) {
            rising_edge = all_pressed && !prev_screenshot_keys;
        }
        prev_screenshot_keys = all_pressed;
        prev_initialized = true;

        if (rising_edge && (now_local - last_screenshot_time_local) > std::chrono::seconds(1)) {
            last_screenshot_time_local = now_local;
            PRINT_DEBUG("Screenshot hotkey triggered");
            _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
        }
#endif
    }

    // Process any captured screenshots and save them to disk
    process_captured_screenshots();

    if (show_overlay) {
        render_main_window();
        render_gallery_window();
    }

    // Pinned screenshot (always rendered when active, click-through when overlay closed)
    render_pinned_screenshot();

    if (notifications.size()) {
        ImGuiIO &io = ImGui::GetIO();
        build_notifications(io.DisplaySize.x, io.DisplaySize.y);
    }
    if (stats.show_any_stats()) {
        int stats_lang = (current_language < TRANSLATION_NUMBER_OF_LANGUAGES) ? current_language : 0;
        stats.render_stats(stats_lang);
    }

    load_next_ach_icon();
}

uint32 Steam_Overlay::apply_global_style_color()
{
    ImVec4 bg_col = (settings->overlay_appearance.background_r >= 0 &&
                     settings->overlay_appearance.background_g >= 0 &&
                     settings->overlay_appearance.background_b >= 0 &&
                     settings->overlay_appearance.background_a >= 0)
        ? ImVec4(settings->overlay_appearance.background_r,
                 settings->overlay_appearance.background_g,
                 settings->overlay_appearance.background_b,
                 settings->overlay_appearance.background_a)
        : ImVec4(0.058f, 0.082f, 0.129f, 0.99f);

    ImVec4 elem_col = (settings->overlay_appearance.element_r >= 0 &&
                       settings->overlay_appearance.element_g >= 0 &&
                       settings->overlay_appearance.element_b >= 0 &&
                       settings->overlay_appearance.element_a >= 0)
        ? ImVec4(settings->overlay_appearance.element_r,
                 settings->overlay_appearance.element_g,
                 settings->overlay_appearance.element_b,
                 settings->overlay_appearance.element_a)
        : ImVec4(0.15f, 0.22f, 0.32f, 1.0f);

    ImVec4 hover_col = (settings->overlay_appearance.element_hovered_r >= 0 &&
                        settings->overlay_appearance.element_hovered_g >= 0 &&
                        settings->overlay_appearance.element_hovered_b >= 0 &&
                        settings->overlay_appearance.element_hovered_a >= 0)
        ? ImVec4(settings->overlay_appearance.element_hovered_r,
                 settings->overlay_appearance.element_hovered_g,
                 settings->overlay_appearance.element_hovered_b,
                 settings->overlay_appearance.element_hovered_a)
        : ImVec4(0.19f, 0.36f, 0.51f, 1.0f);

    ImVec4 active_col = (settings->overlay_appearance.element_active_r >= 0 &&
                         settings->overlay_appearance.element_active_g >= 0 &&
                         settings->overlay_appearance.element_active_b >= 0 &&
                         settings->overlay_appearance.element_active_a >= 0)
        ? ImVec4(settings->overlay_appearance.element_active_r,
                 settings->overlay_appearance.element_active_g,
                 settings->overlay_appearance.element_active_b,
                 settings->overlay_appearance.element_active_a)
        : ImVec4(0.16f, 0.35f, 0.48f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg_col);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.088f, 0.122f, 0.179f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.27f, 0.36f, 0.50f, 0.63f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.06f, 0.08f, 0.12f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, elem_col);
    ImGui::PushStyleColor(ImGuiCol_Button, elem_col);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.15f, 0.22f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, elem_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_col);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hover_col);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, hover_col);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hover_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_col);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, active_col);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, active_col);
    ImGui::PushStyleColor(ImGuiCol_Header, active_col);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, active_col);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(0.15f, 0.21f, 0.30f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.17f, 0.24f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.38f, 0.51f, 0.65f, 0.28f));

    return 20;
}

void Steam_Overlay::render_overlay_controls()
{
    const bool rtl = current_language == PERSIAN_LANG_INDEX;
    if (settings->overlay_show_button_user_info) {
        overlay_checkbox(tr_ui(current_language, translationToggleUserInfo, "اطلاعات بازیکن"), &show_user_info, rtl);
    }
    if (settings->overlay_show_checkbox_fps) {
        if (overlay_checkbox(tr_ui(current_language, translationFpsCheckbox, "نمایش FPS"), &stats.show_fps, rtl)) {
            allow_renderer_frame_processing(stats.show_fps);
        }
    }
    if (settings->overlay_show_checkbox_frametime) {
        if (overlay_checkbox(tr_ui(current_language, translationFrametimeCheckbox, "زمان فریم"), &stats.show_frametime, rtl)) {
            allow_renderer_frame_processing(stats.show_frametime);
        }
    }
    if (settings->overlay_show_checkbox_playtime) {
        if (overlay_checkbox(tr_ui(current_language, translationPlaytimeCheckbox, "تایمر بازی"), &stats.show_playtime, rtl)) {
            allow_renderer_frame_processing(stats.show_playtime);
        }
    }
    if (settings->overlay_show_button_copy_id) {
        if (overlay_button(tr_ui(current_language, translationCopyId, "کپی آیدی استیم"), rtl)) {
            const auto id = std::to_string(settings->get_local_steam_id().ConvertToUint64());
            ImGui::SetClipboardText(id.c_str());
        }
    }
    if (settings->overlay_show_button_test_achievement) {
        if (overlay_button(tr_ui(current_language, translationTestAchievement, "تست دستاورد"), rtl)) {
            show_test_achievement();
        }
    }
}

void Steam_Overlay::render_friends_page(const PeerDiscoverySnapshot& discovery)
{
    const bool rtl = current_language == PERSIAN_LANG_INDEX;
    auto label = [this](const char* english, const char* persian) {
        return overlay_label(current_language, english, persian);
    };
    const auto visible = [&discovery](const auto& entry) {
        return overlay_peer_visible(discovery, entry.first.id());
    };
    const auto visible_count = std::count_if(friends.begin(), friends.end(), visible);
    const auto count = std::to_string(visible_count);
    const std::string title = tr_ui(current_language, translationFriends, "دوستان");
    const auto heading = rtl ? "(" + count + ") " + title : title + " (" + count + ")";
    overlay_line(heading.c_str(), rtl);
    if (i_have_lobby && visible_count > 0
        && overlay_button(tr_ui(current_language, translationInviteAll, "دعوت همه به لابی"), rtl)) {
        invite_all_friends_clicked = true;
    }
    ImGui::Separator();

    if (visible_count == 0) {
        overlay_paragraph(rtl
            ? "هنوز دوستی پیدا نشده است. برای جست‌وجو در شبکه، آدرس دوستتان را اضافه کنید."
            : "No friends have been discovered. Add a friend's address to search over LAN or VPN.", rtl);
        if (overlay_button(label("Add a friend's IP", "افزودن آدرس دوست"), rtl)) {
            active_page = OverlayPage::Network;
        }
    }

    const auto app_id = displayed_app_id;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(rtl ? 1.0f : 0.0f, 0.5f));
    for (auto &[friend_info, state] : friends) {
        if (!overlay_peer_visible(discovery, friend_info.id())) continue;

        ImGui::PushID(state.id);
        const auto display_name = fa_shape(friend_info.name());
        const auto row_label = display_name + "###friend_name";
        ImGui::Selectable(row_label.c_str(), (state.window_state & window_state_show) != 0,
            ImGuiSelectableFlags_AllowDoubleClick);
        const bool open_chat = ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        build_friend_context_menu(friend_info, state);
        if (open_chat) state.window_state |= window_state_show;

        if (!rtl) ImGui::Indent();
        const bool same_game = friend_info.appid() == app_id;
        const bool can_join = same_game && state.joinable;
        const std::string status = can_join ? label("Ready to join", "آماده پیوستن")
            : same_game ? label("Playing this game", "در حال اجرای همین بازی")
            : "AppID " + std::to_string(friend_info.appid());
        ImGui::PushStyleColor(ImGuiCol_Text, can_join ? ImVec4(0.36f, 0.82f, 0.69f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        overlay_line(status.c_str(), rtl);
        ImGui::PopStyleColor();

        const char* chat = tr_ui(current_language, translationChat, "گفت‌وگو");
        const bool has_game_action = same_game && (i_have_lobby || can_join);
        const char* action = can_join
            ? tr_ui(current_language, translationJoin, "پیوستن")
            : tr_ui(current_language, translationInvite, "دعوت");
        auto render_game_action = [&] {
            if (ImGui::SmallButton(action)) {
                state.window_state |= can_join ? window_state_join : window_state_invite;
                has_friend_action.push(friend_info);
            }
        };
        if (rtl) {
            float width = ImGui::CalcTextSize(chat).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (has_game_action) {
                width += ImGui::CalcTextSize(action).x + ImGui::GetStyle().FramePadding.x * 2.0f
                    + ImGui::GetStyle().ItemSpacing.x;
            }
            align_overlay_item_right(width);
            if (has_game_action) {
                render_game_action();
                ImGui::SameLine();
            }
        }
        if (ImGui::SmallButton(chat)) state.window_state |= window_state_show;
        if (!rtl && has_game_action) {
            ImGui::SameLine();
            render_game_action();
        }
        if (!rtl) ImGui::Unindent();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    if (!settings->overlay_show_button_settings
        && ImGui::CollapsingHeader(label("Display and shortcuts", "نمایش و میانبرها"))) {
        render_overlay_controls();
    }
}

void Steam_Overlay::render_network_page(const PeerDiscoverySnapshot& discovery)
{
    const bool rtl = current_language == PERSIAN_LANG_INDEX;
    auto label = [this](const char* english, const char* persian) {
        return overlay_label(current_language, english, persian);
    };
    const ImVec4 accent(0.42f, 0.77f, 0.93f, 1.0f);
    const ImVec4 warning(0.98f, 0.73f, 0.39f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    overlay_line(label("Find a friend by IP", "پیدا کردن دوست با آدرس"), rtl);
    ImGui::PopStyleColor();
    overlay_paragraph(rtl
        ? "نشانی دوستتان در شبکه داخلی را وارد کنید. اگر پورت را خالی بگذارید، از 47584 استفاده می‌شود."
        : "Enter your friend's IPv4 address on the LAN or VPN. Leave the port blank to use 47584.", rtl);
    ImGui::Spacing();

    const char* add_label = label("Find friend", "پیدا کردن دوست");
    bool submitted = false;
    auto address_input = [&] {
        overlay_line(label("IPv4 address", "نشانی آی‌پی"), rtl);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##peer_address", "10.0.0.15", peer_address,
            sizeof(peer_address), ImGuiInputTextFlags_EnterReturnsTrue)) submitted = true;
    };
    auto port_input = [&] {
        overlay_line(label("Port", "پورت"), rtl);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##peer_port", "47584", peer_port,
            sizeof(peer_port), ImGuiInputTextFlags_EnterReturnsTrue)) submitted = true;
    };
    auto add_button = [&] {
        ImGui::BeginDisabled(!network);
        if (overlay_button(add_label, rtl)) submitted = true;
        ImGui::EndDisabled();
    };

    const float button_width = ImGui::CalcTextSize(add_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float min_inline_width = button_width + ImGui::GetFontSize() * 14.0f;
    if (ImGui::GetContentRegionAvail().x >= min_inline_width
        && ImGui::BeginTable("##peer_form", 3, ImGuiTableFlags_SizingStretchProp)) {
        const int ip_col = rtl ? 2 : 0;
        const int port_col = 1;
        const int action_col = rtl ? 0 : 2;
        auto setup_action = [&] {
            ImGui::TableSetupColumn("##submit", ImGuiTableColumnFlags_WidthFixed, button_width);
        };
        auto setup_ip = [] {
            ImGui::TableSetupColumn("##address", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        };
        if (rtl) setup_action();
        else setup_ip();
        ImGui::TableSetupColumn("##port", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        if (rtl) setup_ip();
        else setup_action();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(ip_col);
        address_input();
        ImGui::TableSetColumnIndex(port_col);
        port_input();
        ImGui::TableSetColumnIndex(action_col);
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
        add_button();
        ImGui::EndTable();
    } else {
        address_input();
        port_input();
        add_button();
    }

    if (network && submitted) {
        peer_action_error.clear();
        std::string endpoint = normalize_ip_field(peer_address);
        const std::string port = normalize_ip_field(peer_port);
        if (endpoint.find(':') != std::string::npos) {
            peer_action_error = rtl ? "پورت را در کادر جداگانه وارد کنید." : "Enter the port in the separate Port field.";
        } else {
            if (!port.empty()) endpoint += ':' + port;
            if (network->add_custom_endpoint(endpoint, peer_action_error)) {
                peer_address[0] = '\0';
                peer_port[0] = '\0';
            }
        }
    }

    if (!peer_action_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, warning);
        if (rtl) overlay_line(label("Address update failed", "به‌روزرسانی آدرس انجام نشد"), true);
        overlay_paragraph(peer_action_error.c_str(), rtl);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!network) {
        ImGui::PushStyleColor(ImGuiCol_Text, warning);
        overlay_line(label("Network is unavailable.", "شبکه در دسترس نیست."), rtl);
        ImGui::PopStyleColor();
        return;
    }

    bool automatic = discovery.automatic_discovery;
    if (overlay_checkbox(label("Discover nearby players automatically", "کشف خودکار بازیکنان در شبکه"), &automatic, rtl)) {
        network->set_automatic_discovery(automatic);
    }
    if (!automatic) {
        overlay_paragraph(rtl
            ? "فقط بازیکنانی که دستی اضافه می‌کنید در فهرست دوستان نمایش داده می‌شوند."
            : "Friends shows only players you add by address.", rtl);
    }

    if (discovery.enabled) {
        const std::string ports = "UDP " + std::to_string(discovery.udp_port) + "   /   TCP " + std::to_string(discovery.tcp_port);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        overlay_line(ports.c_str(), rtl);
        ImGui::PopStyleColor();
        const auto now = std::chrono::steady_clock::now();
        if (now >= refresh_local_addresses_at) {
            local_addresses = network->get_local_vpn_ips();
            refresh_local_addresses_at = now + std::chrono::seconds(5);
        }
        if (!local_addresses.empty() && ImGui::CollapsingHeader(label("Addresses on this PC", "آدرس‌های این دستگاه"))) {
            for (const auto &address : local_addresses) {
                const std::string endpoint = address + ":" + std::to_string(discovery.udp_port);
                const char* copy = label("Copy", "کپی");
                ImGui::PushID(endpoint.c_str());
                if (rtl) {
                    const float width = ImGui::CalcTextSize(copy).x + ImGui::CalcTextSize(endpoint.c_str()).x
                        + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
                    align_overlay_item_right(width);
                    if (ImGui::SmallButton(copy)) ImGui::SetClipboardText(endpoint.c_str());
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(endpoint.c_str());
                if (!rtl) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(copy)) ImGui::SetClipboardText(endpoint.c_str());
                }
                ImGui::PopID();
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, warning);
        overlay_paragraph(rtl
            ? "جست‌وجوی شبکه در این بازی فعال نیست یا در دسترس نیست."
            : "Network discovery is disabled or unavailable for this game.", rtl);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const auto visible_peer = [automatic](const auto& peer) { return automatic || peer.editable; };
    if (std::none_of(discovery.peers.begin(), discovery.peers.end(), visible_peer)) {
        if (discovery.enabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            overlay_line(label("No saved addresses", "آدرس ذخیره‌شده‌ای وجود ندارد"), rtl);
            ImGui::PopStyleColor();
        }
        return;
    }
    if (!ImGui::BeginTable("##peer_status", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;

    const int address_col = rtl ? 2 : 0;
    const int status_col = 1;
    const int action_col = rtl ? 0 : 2;
    const char* address_title = label("Address", "آدرس");
    const char* status_title = label("Status", "وضعیت");
    const char* remove = label("Remove", "حذف");
    auto setup_address = [&] { ImGui::TableSetupColumn(address_title, ImGuiTableColumnFlags_WidthStretch, 1.05f); };
    auto setup_action = [&] {
        ImGui::TableSetupColumn("##peer_actions", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize(remove).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    };
    if (rtl) setup_action();
    else setup_address();
    ImGui::TableSetupColumn(status_title, ImGuiTableColumnFlags_WidthStretch, 1.35f);
    if (rtl) setup_address();
    else setup_action();
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(address_col);
    overlay_line(address_title, rtl);
    ImGui::TableSetColumnIndex(status_col);
    overlay_line(status_title, rtl);

    for (const auto &peer : discovery.peers) {
        if (!visible_peer(peer)) continue;

        const char* status = label("Waiting to search", "در انتظار جست‌وجو");
        ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        switch (peer.state) {
            case PeerDiscoveryState::Waiting:
                if (!discovery.enabled) status = label("Unavailable", "در دسترس نیست");
                break;
            case PeerDiscoveryState::Searching:
                status = label("Searching", "در حال جست‌وجو");
                color = accent;
                break;
            case PeerDiscoveryState::Discovered:
                status = label("Peer discovered", "همتا پیدا شد");
                color = accent;
                break;
            case PeerDiscoveryState::Connected:
                status = label("Peer connected", "همتا متصل شد");
                color = ImVec4(0.36f, 0.82f, 0.69f, 1.0f);
                break;
            case PeerDiscoveryState::SendFailed:
                status = label("Send failed", "ارسال ناموفق");
                color = warning;
                break;
        }

        ImGui::PushID(peer.endpoint.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(address_col);
        overlay_line(peer.endpoint.c_str(), rtl);
        ImGui::TableSetColumnIndex(status_col);
        const auto error = peer.socket_error ? "(" + std::to_string(peer.socket_error) + ")" : std::string{};
        const auto status_text = error.empty() ? status : rtl ? error + " " + status : status + (" " + error);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        overlay_line(status_text.c_str(), rtl);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(action_col);
        if (peer.editable && ImGui::SmallButton(remove)) {
            peer_action_error.clear();
            network->remove_custom_endpoint(peer.endpoint, peer_action_error);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void Steam_Overlay::render_main_window()
{
    const bool is_fa = (current_language == PERSIAN_LANG_INDEX);
    auto label = [this](const char* english, const char* persian) {
        return overlay_label(current_language, english, persian);
    };

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 40.0f || display.y < 40.0f) return;
    const ImVec2 size(std::min(display.x - 20.0f, 1060.0f), std::min(display.y - 20.0f, 710.0f));
    const bool wide = size.x >= 740.0f;
    bool show = true;

    ImGui::PushFont(font_default);
    const uint32 style_color_stack = apply_global_style_color();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(5, 8, 14, 151));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("SteamEM###MainOverlayWnd", nullptr, flags)) {
        const char* close = tr_ui(current_language, translationClose, "بستن");
        const float close_width = ImGui::CalcTextSize(close).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float title_width = ImGui::CalcTextSize("SteamEM").x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float header_width = ImGui::GetContentRegionAvail().x;
        const float header_right = ImGui::GetCursorPosX() + header_width;
        const float name_width = header_width - close_width - title_width - gap * 2.0f;
        const auto local_name = wide ? fa_shape(displayed_local_name) : std::string{};
        const float local_name_width = ImGui::CalcTextSize(local_name.c_str()).x;
        const bool name_fits = wide && !local_name.empty() && local_name_width <= name_width;
        auto render_title = [] {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.56f, 0.83f, 1.0f, 1.0f), "SteamEM");
        };
        if (is_fa) {
            if (ImGui::Button(close)) show = false;
            if (name_fits) {
                ImGui::SameLine(header_right - title_width - gap - local_name_width);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", local_name.c_str());
            }
            ImGui::SameLine(std::max(0.0f, header_right - title_width));
            render_title();
        } else {
            render_title();
            if (name_fits) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", local_name.c_str());
            }
            ImGui::SameLine(std::max(0.0f, header_right - close_width));
            if (ImGui::Button(close)) show = false;
        }

        if (show_user_info) {
            const auto* renderer_name = _renderer ? _renderer->GetLibraryName() : "";
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            char details[200]{};
            snprintf(details, sizeof(details), "SteamID %" PRIu64 "  /  AppID %u  /  %s",
                settings->get_local_steam_id().ConvertToUint64(), displayed_app_id, renderer_name);
            overlay_paragraph(details, is_fa);
            ImGui::PopStyleColor();
            if (settings->record_playtime && playtime_counter && settings->overlay_appearance.show_playtime_in_user_info) {
                const uint64_t total = playtime_counter->seconds();
                const uint64_t session = playtime_counter->session_seconds();
                char total_text[32]{};
                char session_text[32]{};
                snprintf(total_text, sizeof(total_text), "%02u:%02u", static_cast<unsigned>(total / 3600),
                    static_cast<unsigned>(total / 60 % 60));
                snprintf(session_text, sizeof(session_text), "%02u:%02u:%02u", static_cast<unsigned>(session / 3600),
                    static_cast<unsigned>(session / 60 % 60), static_cast<unsigned>(session % 60));
                char duration[256]{};
                snprintf(duration, sizeof(duration), is_fa ? "زمان کل %s  /  نشست فعلی %s"
                    : translationTotalTimeText[current_language], total_text, session_text);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                overlay_paragraph(duration, is_fa);
                ImGui::PopStyleColor();
            }
        }
        ImGui::Separator();

        const struct {
            OverlayPage page;
            const char* title;
            bool visible;
        } pages[] = {
            { OverlayPage::Friends, tr_ui(current_language, translationFriends, "دوستان"), true },
            { OverlayPage::Network, label("Network", "شبکه"), true },
            { OverlayPage::Achievements, tr_ui(current_language, translationAchievements, "دستاوردها"), settings->overlay_show_button_achievements },
            { OverlayPage::Activity, tr_ui(current_language, translationHistory, "تاریخچه"), settings->overlay_show_button_history },
            { OverlayPage::Settings, tr_ui(current_language, translationSettings, "تنظیمات"), settings->overlay_show_button_settings },
        };
        const char* current_page = pages[0].title;
        for (const auto &page : pages) {
            if (page.page == active_page && page.visible) current_page = page.title;
        }
        const char* screenshots = tr_ui(current_language, translationScreenshots, "تصاویر بازی");

        const float side_width = std::clamp(size.x * 0.23f, 164.0f, 224.0f);
        const auto render_sidebar = [&] {
            ImGui::BeginChild("##overlay_navigation", ImVec2(side_width, 0), true);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(is_fa ? 0.94f : 0.06f, 0.5f));
            for (const auto &page : pages) {
                if (!page.visible) continue;
                ImGui::PushID(static_cast<int>(page.page));
                const auto color = page.page == active_page ? ImGuiCol_ButtonActive : ImGuiCol_ChildBg;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(color));
                if (ImGui::Button(page.title, ImVec2(-1, ImGui::GetFrameHeight() + 7.0f))) active_page = page.page;
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (settings->overlay_show_button_screenshots) {
                ImGui::Separator();
                if (ImGui::Button(screenshots, ImVec2(-1, ImGui::GetFrameHeight() + 7.0f))) {
                    show_screenshots_window = !show_screenshots_window;
                }
            }
            ImGui::PopStyleVar();
            ImGui::EndChild();
        };
        if (wide && !is_fa) {
            render_sidebar();
            ImGui::SameLine();
        } else if (!wide) {
            const float screenshots_width = ImGui::CalcTextSize(screenshots).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const bool inline_gallery = settings->overlay_show_button_screenshots
                && ImGui::GetContentRegionAvail().x > screenshots_width + 130.0f;
            const float selector_width = inline_gallery
                ? ImGui::GetContentRegionAvail().x - screenshots_width - ImGui::GetStyle().ItemSpacing.x
                : -1.0f;
            if (is_fa && inline_gallery) {
                if (ImGui::Button(screenshots)) show_screenshots_window = !show_screenshots_window;
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(selector_width);
            if (ImGui::BeginCombo("##overlay_navigation", current_page)) {
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(is_fa ? 1.0f : 0.0f, 0.5f));
                for (const auto &page : pages) {
                    if (page.visible && ImGui::Selectable(page.title, page.page == active_page)) active_page = page.page;
                }
                ImGui::PopStyleVar();
                ImGui::EndCombo();
            }
            if (settings->overlay_show_button_screenshots && (!is_fa || !inline_gallery)) {
                if (inline_gallery) ImGui::SameLine();
                if (overlay_button(screenshots, is_fa)) show_screenshots_window = !show_screenshots_window;
            }
        }

        const float body_width = wide && is_fa
            ? ImGui::GetContentRegionAvail().x - side_width - ImGui::GetStyle().ItemSpacing.x
            : 0.0f;
        const auto discovery = network ? network->peer_discovery_snapshot() : PeerDiscoverySnapshot{};
        ImGui::BeginChild("##overlay_content", ImVec2(body_width, 0.0f), true);
        if (active_page == OverlayPage::Friends) {
            render_friends_page(discovery);
        } else if (active_page == OverlayPage::Network) {
            render_network_page(discovery);
        } else if (active_page == OverlayPage::Achievements) {
            if (achievements.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                overlay_paragraph(is_fa ? "هیچ دستاوردی برای این بازی ثبت نشده است."
                    : "No achievements configured for this game.", is_fa);
                ImGui::PopStyleColor();
            } else {
                std::vector<size_t> unlocked_idx, locked_idx;
                for (size_t i = 0; i < achievements.size(); ++i) {
                    if (achievements[i].achieved) unlocked_idx.push_back(i);
                    else locked_idx.push_back(i);
                }

                float ratio = static_cast<float>(unlocked_idx.size()) / static_cast<float>(achievements.size());
                char prog_overlay[64]{};
                snprintf(prog_overlay, sizeof(prog_overlay), "%zu / %zu (%.1f%%)", unlocked_idx.size(), achievements.size(), ratio * 100.0f);
                ImGui::ProgressBar(ratio, ImVec2(-1.0f, 22.0f), prog_overlay);
                ImGui::Spacing();

                std::sort(unlocked_idx.begin(), unlocked_idx.end(), [this](size_t a, size_t b) {
                    return achievements[a].unlock_time > achievements[b].unlock_time;
                });

                auto render_ach = [this, is_fa](Overlay_Achievement &x) {
                    const bool achieved = x.achieved;
                    const bool hidden = x.hidden && !achieved;
                    try_load_ach_icon(x, achieved, settings->paginated_achievements_icons == 0);

                    ImGui::Separator();
                    bool could_create_ach_table_entry = false;
                    if (x.icon->GetResourceId() != 0 || x.icon_gray->GetResourceId() != 0) {
                        if (ImGui::BeginTable(x.name.c_str(), 2)) {
                            could_create_ach_table_entry = true;
                            ImGui::TableSetupColumn("img", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                            ImGui::TableSetupColumn("txt");
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);
                            ImGui::TableSetColumnIndex(0);
                            auto &icon_rsrc = achieved ? x.icon : x.icon_gray;
                            if (icon_rsrc->GetResourceId() != 0) {
                                ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size));
                            }
                            ImGui::TableSetColumnIndex(1);
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, achieved
                        ? ImVec4(0.4f, 0.95f, 0.5f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
                    overlay_line(fa_shape(x.title).c_str(), is_fa);
                    ImGui::PopStyleColor();

                    if (hidden) {
                        ImGui::TextDisabled("%s", tr_ui(current_language, translationHiddenAchievement, "دستاورد مخفی"));
                        ImGui::SameLine();
                        ImGui::PushID(&x);
                        ImGui::SmallButton(tr_ui(current_language, translationShow, "نمایش"));
                        bool show_desc = ImGui::IsItemActive();
                        ImGui::PopID();
                        if (show_desc) overlay_paragraph(x.description.c_str(), is_fa);
                    } else {
                        overlay_paragraph(x.description.c_str(), is_fa);
                    }

                    if (achieved) {
                        char buffer[80]{};
                        time_t unlock_time = (time_t)x.unlock_time;
                        struct tm unlock_tm{};
#ifdef _MSC_VER
                        localtime_s(&unlock_tm, &unlock_time);
#else
                        localtime_r(&unlock_time, &unlock_tm);
#endif
                        std::strftime(buffer, sizeof(buffer), "%Y/%m/%d - %H:%M:%S", &unlock_tm);
                        if (is_fa) {
                            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "%s %s", fa_shape("باز شده در تاریخ:").c_str(), buffer);
                        } else {
                            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), translationAchievedOn[current_language], buffer);
                        }
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tr_ui(current_language, translationNotAchieved, "قفل شده"));
                    }

                    add_ach_progressbar(x);
                    if (could_create_ach_table_entry) ImGui::EndTable();
                };

                if (ImGui::CollapsingHeader(tr_ui(current_language, translationUnlocked, "دستاوردهای باز شده"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (unlocked_idx.empty()) ImGui::TextDisabled("%s", tr_ui(current_language, translationNoUnlockedAchievements, "هنوز هیچ دستاوردی باز نشده است."));
                    else for (auto idx : unlocked_idx) render_ach(achievements[idx]);
                }
                if (ImGui::CollapsingHeader(tr_ui(current_language, translationLocked, "دستاوردهای قفل شده"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (locked_idx.empty()) ImGui::TextDisabled("%s", tr_ui(current_language, translationAllAchievementsUnlocked, "تمام دستاوردها باز شده‌اند!"));
                    else for (auto idx : locked_idx) render_ach(achievements[idx]);
                }
            }
        }

        // -------------------- تب ۳: تاریخچه اعلان‌ها --------------------
        else if (active_page == OverlayPage::Activity) {
            if (overlay_button(tr_ui(current_language, translationClearAll, "پاک کردن همه"), is_fa)) {
                notification_history.clear();
                notification_history_cache.clear();
                notification_history_cache_dirty = false;
            }
            ImGui::Separator();
            if (notification_history.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                overlay_line(tr_ui(current_language, translationNoNotification, "هیچ اعلانی ثبت نشده است."), is_fa);
                ImGui::PopStyleColor();
            } else {
                for (auto it = notification_history.rbegin(); it != notification_history.rend(); ++it) {
                    const time_t total_sec = std::chrono::duration_cast<std::chrono::seconds>(it->timestamp).count();
                    struct tm local_tm_buf{};
#ifdef _MSC_VER
                    localtime_s(&local_tm_buf, &total_sec);
#else
                    localtime_r(&total_sec, &local_tm_buf);
#endif
                    char time_prefix[32]{};
                    snprintf(time_prefix, sizeof(time_prefix), "[%02d:%02d:%02d]", local_tm_buf.tm_hour, local_tm_buf.tm_min, local_tm_buf.tm_sec);
                    const std::string history_line = std::string(time_prefix) + " " + it->message;
                    overlay_paragraph(history_line.c_str(), is_fa);
                    ImGui::Separator();
                }
            }
        }

        // -------------------- تب ۴: تنظیمات (شامل انتخاب زبان فارسی) --------------------
        else if (active_page == OverlayPage::Settings) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 1.0f, 1.0f));
            overlay_paragraph(is_fa ? "تنظیمات کلی شبیه‌ساز و زبان رابط کاربری"
                : translationGlobalSettingsWindowDescription[current_language], is_fa);
            ImGui::PopStyleColor();
            ImGui::Separator();

            overlay_line(tr_ui(current_language, translationUsername, "نام کاربری"), is_fa);
            const float settings_field_width = std::min(360.0f, ImGui::GetContentRegionAvail().x);
            if (is_fa) align_overlay_item_right(settings_field_width);
            ImGui::SetNextItemWidth(settings_field_width);
            ImGui::InputText("##username", username_text, sizeof(username_text), 0);

            ImGui::Spacing();
            ImGui::Separator();

            overlay_line(tr_ui(current_language, translationLanguage, "زبان"), is_fa);
            const auto language_name = [](int index) {
                return overlay_label(index, valid_languages[index], "فارسی");
            };
            if (is_fa) align_overlay_item_right(settings_field_width);
            ImGui::SetNextItemWidth(settings_field_width);
            if (ImGui::BeginCombo("##language", language_name(current_language))) {
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(is_fa ? 1.0f : 0.0f, 0.5f));
                for (int index = 0; index < static_cast<int>(std::size(valid_languages)); ++index) {
                    if (ImGui::Selectable(language_name(index), index == current_language)) current_language = index;
                }
                ImGui::PopStyleVar();
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            overlay_paragraph(is_fa ? "برای اعمال کامل تنظیمات بازی را دوباره باز کنید."
                : translationRestartTheGameToApply[current_language], is_fa);
            ImGui::PopStyleColor();
            if (overlay_button(tr_ui(current_language, translationSave, "ذخیره تنظیمات"), is_fa)) {
                save_settings = true;
            }
            ImGui::Spacing();
            ImGui::Separator();
            render_overlay_controls();
        }

        ImGui::EndChild();

        if (wide && is_fa) {
            ImGui::SameLine();
            render_sidebar();
        }
        for (auto &[friend_info, state] : friends) {
            if (overlay_peer_visible(discovery, friend_info.id())) build_friend_window(friend_info, state);
        }

        // نمایش پنجره URL در صورت فراخوانی از سوی بازی
        if (show_url.size()) {
            std::string url = show_url;
            bool url_open = true;
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(URL_WINDOW_NAME, &url_open)) {
                ImGui::Text("%s", tr_ui(current_language, translationSteamOverlayURL, "آدرس وب:"));
                ImGui::Spacing();
                ImGui::PushItemWidth(ImGui::CalcTextSize(url.c_str()).x + 20);
                ImGui::InputText("##url_copy", url.data(), url.size() + 1, ImGuiInputTextFlags_ReadOnly);
                ImGui::PopItemWidth();
                ImGui::Spacing();
                if (ImGui::Button(tr_ui(current_language, translationClose, "بستن")) || !url_open)
                    show_url = "";
            }
            ImGui::End();
        }

        // پنجره هشدار در صورت AppID=0 یا ذخیره محلی
        bool show_warning = warn_local_save || warn_bad_appid;
        if (show_warning) {
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(tr_ui(current_language, translationWarning, "هشدار"), &show_warning)) {
                if (warn_bad_appid) {
                    ImGui::TextWrapped("%s", tr_ui(current_language, translationWarningDescription_badAppid, "هشدار: AppID بازی صفر است!"));
                }
                if (warn_local_save) {
                    ImGui::TextWrapped("%s", tr_ui(current_language, translationWarningDescription_localSave, "هشدار: ذخیره‌سازی محلی فعال است."));
                }
            }
            ImGui::End();
            if (!show_warning) {
                warn_local_save = false;
                warn_bad_appid = false;
            }
        }
    }

    ImGui::End();

    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(style_color_stack);
    ImGui::PopFont();

    if (!show) {
        ShowOverlay(false);
    }
}
void Steam_Overlay::load_next_ach_icon()
{
    // this function only works when icons pagination is active, request-based loading is not supported too (pagination=0)
    if (!settings->overlay_upload_achs_icons_to_gpu || settings->paginated_achievements_icons <= 0 || achievements.empty()) return;

    std::unique_lock client_lock(global_mutex, std::try_to_lock);
    if (!client_lock) return;

    size_t linear_idx = last_loaded_ach_icon / 2; // 2 icons per achievement, 1 achieved, 1 unachieved
    if (linear_idx >= achievements.size()) {
        last_loaded_ach_icon = 0;
        linear_idx = 0;
    }

#ifndef EMU_RELEASE_BUILD
    auto now1 = std::chrono::high_resolution_clock::now();
#endif

    auto &ach = achievements.at(linear_idx);
    ++last_loaded_ach_icon;

    bool achieved = last_loaded_ach_icon % 2 != 0;
    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    // always force upload to GPU in background-loading mode (pagination > 0)
    bool loaded = try_load_ach_icon(ach, achieved, true);

#ifndef EMU_RELEASE_BUILD
    if (loaded) {
        auto now2 = std::chrono::high_resolution_clock::now();
        auto dd = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
        PRINT_DEBUG("uploaded an achievement icon to GPU in %u ms", dd);
    }
#endif

}

void Steam_Overlay::SetupOverlay()
{
    if (settings->disable_overlay) {
        log_overlay_startup("Overlay disabled. Check enable_experimental_overlay in configs.overlay.ini.");
        return;
    }

    PRINT_DEBUG_ENTRY();
    std::lock_guard lifecycle_lock(overlay_lifecycle_mutex);
    if (setup_overlay_called.exchange(true)) return;

    log_overlay_startup("Overlay starting. Waiting for renderer detection.");
    if (settings->overlay_hook_delay_sec > 0) {
        PRINT_DEBUG("waiting %i seconds", settings->overlay_hook_delay_sec);
        renderer_detector_delay_thread.start();
    } else {
        // Some games require renderer detection to start before returning from SteamAPI_Init.
        request_renderer_detector();
        renderer_hook_init_thread.start();
    }
}

void Steam_Overlay::UnSetupOverlay()
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard lifecycle_lock(overlay_lifecycle_mutex);
    if (!setup_overlay_called.exchange(false)) return;

    // The delayed worker can still start the initialization worker.
    // Both workers must be idle before taking overlay_mutex and tearing down the renderer.
    renderer_detector_delay_thread.kill();
    renderer_hook_init_thread.kill();

    std::lock_guard lock(overlay_mutex);
    is_ready = false;
    renderer_hook_installed = false;
    late_init_imgui = false;
    show_overlay = false;
    overlay_requested = false;
    overlay_state_changed = false;
    startup_ready_logged = false;

    if (_renderer) {
        // Some games release the graphics device after shutting down Steamworks.
        _renderer->OverlayHookReady = [](InGameOverlay::OverlayHookState){};
        _renderer->OverlayProc = [](){};
        _renderer->SetScreenshotCallback(nullptr, nullptr);

        allow_renderer_frame_processing(false, true);
        obscure_game_input(false);

        for (auto &ach : achievements) {
            for (auto *resource : {ach.icon, ach.icon_gray}) {
                if (!resource) continue;
                if (resource->GetResourceId() != 0) resource->Unload();
                resource->Delete();
            }
        }

        for (auto &item : screenshot_items) {
            if (item.texture) {
                if (item.texture->GetResourceId() != 0) item.texture->Unload();
                item.texture->Delete();
            }
        }
        screenshot_items.clear();
        preview_pixels.clear();
        preview_pixels_w = 0;
        preview_pixels_h = 0;
        if (preview_texture) {
            if (preview_texture->GetResourceId() != 0) preview_texture->Unload();
            preview_texture->Delete();
            preview_texture = nullptr;
        }
        unpin_all_screenshots();

        // Remove the hook before DX12 game shutdown or the process can crash (appid 2933080, 1583230).
        _renderer->~RendererHook_t();
        _renderer = nullptr;
    }

    renderer_frame_processing_requests = 0;
    obscure_cursor_requests = 0;
    achievements.clear();
    achievement_queue.clear();
    notifications.clear();
    last_loaded_ach_icon = 0;
    fonts_atlas.Clear();
    cleanup_renderer_hook();
    log_overlay_startup("Overlay stopped.");

    PRINT_DEBUG("done *********");
}

bool Steam_Overlay::Ready() const
{
    return setup_overlay_called && is_ready && late_init_imgui;
}

bool Steam_Overlay::NeedPresent() const
{
    PRINT_DEBUG_ENTRY();
    return !settings->disable_overlay;
}

void Steam_Overlay::SetNotificationPosition(ENotificationPosition eNotificationPosition)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("TODO %i", (int)eNotificationPosition);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    notif_position = eNotificationPosition;
}

void Steam_Overlay::SetNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("TODO x=%i y=%i", nHorizontalInset, nVerticalInset);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    h_inset = nHorizontalInset;
    v_inset = nVerticalInset;
}

void Steam_Overlay::OpenOverlayInvite(CSteamID lobbyId)
{
    PRINT_DEBUG("%llu", lobbyId.ConvertToUint64());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    // remember the lobby the game wants the invitations to be sent to,
    // the actual invite happens later when the user clicks "invite" on a friend
    if (lobbyId.IsValid()) {
        invite_lobby_id = lobbyId.ConvertToUint64();
    }
    ShowOverlay(true);
}

void Steam_Overlay::OpenOverlay(const char* pchDialog)
{
    PRINT_DEBUG("TODO '%s'", pchDialog ? pchDialog : "");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    // TODO: Show pages depending on pchDialog
    if (pchDialog && (strncmp(pchDialog, "Friends", sizeof("Friends") - 1) == 0) && (settings->overlayAutoAcceptInvitesCount() > 0)) {
        PRINT_DEBUG("won't open overlay's friends list because some friends are defined in the auto accept list");
        add_auto_accept_invite_notification();
    } else {
        ShowOverlay(true);
    }
}

void Steam_Overlay::OpenOverlayWebpage(const char* pchURL)
{
    if (!pchURL) return;
    PRINT_DEBUG("TODO '%s'", pchURL);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    show_url = pchURL;
    ShowOverlay(true);
}

bool Steam_Overlay::ShowOverlay() const
{
    return show_overlay;
}

void Steam_Overlay::ShowOverlay(bool state)
{
    if (setup_overlay_called) overlay_requested = state;
}

void Steam_Overlay::SetLobbyInvite(Friend friendId, uint64 lobbyId)
{
    PRINT_DEBUG("%" PRIu64 " %llu", friendId.id(), lobbyId);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        auto& frd = i->second;
        frd.lobbyId = lobbyId;
        frd.window_state |= window_state_lobby_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_rich_invite;
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::SetRichInvite(Friend friendId, const char* connect_str)
{
    PRINT_DEBUG("%" PRIu64 " '%s'", friendId.id(), connect_str);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        auto& frd = i->second;
        strncpy(frd.connect, connect_str, k_cchMaxRichPresenceValueLength - 1);
        frd.window_state |= window_state_rich_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_lobby_invite;
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::FriendConnect(Friend _friend)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;

    int id = find_free_friend_id(friends);
    if (id != 0) {
        auto& item = friends[_friend];
        item.window_title = std::move(_friend.name() + " " + translationPlaying[current_language] + " " + std::to_string(_friend.appid()));
        item.window_state = window_state_none;
        item.id = id;
        memset(item.chat_input, 0, max_chat_len);
        item.joinable = false;
    } else {
        PRINT_DEBUG("error no free id to create a friend window");
    }
}

void Steam_Overlay::FriendDisconnect(Friend _friend)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;

    auto it = friends.find(_friend);
    if (it != friends.end())
        friends.erase(it);
}

// show a notification when the user unlocks an achievement
void Steam_Overlay::AddAchievementNotification(const std::string &ach_name, nlohmann::json const &ach, bool for_progress)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("'%s' %i", ach_name.c_str(), (int)for_progress);
    std::lock_guard client_lock(global_mutex);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // Phase 1: Update achievement data regardless of Ready() state.
    // This keeps the achievement list accurate even if the overlay isn't
    // fully initialized yet (e.g., during the startup window before late_init_imgui).
    for (auto &a : achievements) {
        if (a.name == ach_name) {
            try {
                a.achieved = ach.value("earned", false);
                a.unlock_time = ach.value("earned_time", static_cast<uint32>(0));
                a.progress = ach.value("progress", static_cast<uint32>(0));
                a.max_progress = ach.value("max_progress", static_cast<uint32>(0));
            } catch(...) {}

            // Phase 2: Only show notification if overlay is ready
            if (!Ready()) return;

            if (a.achieved && !for_progress) { // here we don't show the progress indications
                post_achievement_notification(a, for_progress);
                // sound is now played when notification is actually shown (delayed with queue)
            } else if (for_progress && !settings->disable_overlay_achievement_progress) { // progress indication is shown for locked achievements only
                // post notification if this isn't a progress, or a progress and the user didn't disable these notifications
                post_achievement_notification(a, for_progress);
                // don't play sound
            }
            break;
        }
    }
}



// -- steam run callbacks --
void Steam_Overlay::steam_run_callback_update_my_lobby()
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    if (std::string(steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect")).length() > 0) {
        i_have_lobby = true;
    } else if (settings->get_lobby().IsValid()) {
        i_have_lobby = true;
    } else {
        i_have_lobby = false;
    }
}

bool Steam_Overlay::is_friend_joinable(std::pair<const Friend, friend_window_state> &f)
{
    PRINT_DEBUG("%" PRIu64 "", f.first.id());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    if (std::string(steamFriends->get_friend_rich_presence_silent((uint64)f.first.id(), "connect")).length() > 0 ) {
        PRINT_DEBUG("%" PRIu64 " true (connect string)", f.first.id());
        return true;
    }

    FriendGameInfo_t friend_game_info{};
    steamFriends->GetFriendGamePlayed((uint64)f.first.id(), &friend_game_info);
    if (friend_game_info.m_steamIDLobby.IsValid() && (f.second.window_state & window_state_lobby_invite)) {
        PRINT_DEBUG("%" PRIu64 " true (friend in a game)", f.first.id());
        return true;
    }

    PRINT_DEBUG("%" PRIu64 " false", f.first.id());
    return false;
}

void Steam_Overlay::invite_friend(uint64 friend_id, class Steam_Friends* steamFriends, class Steam_Matchmaking* steamMatchmaking)
{
    std::string connect_str = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
    if (connect_str.length() > 0) {
        steamFriends->InviteUserToGame(friend_id, connect_str.c_str());
        PRINT_DEBUG("sent game invitation to friend with id = %llu", friend_id);
    } else {
        // prefer the lobby the game explicitly passed to OpenOverlayInvite(),
        // otherwise fall back to the last joined lobby
        CSteamID lobby_id( invite_lobby_id.load(std::memory_order_relaxed) );
        if (!lobby_id.IsValid()) {
            lobby_id = settings->get_lobby();
        }

        if (lobby_id.IsValid()) {
            steamMatchmaking->InviteUserToLobby(lobby_id, friend_id);
            PRINT_DEBUG("sent lobby invitation (%llu) to friend with id = %llu", lobby_id.ConvertToUint64(), friend_id);
        }
    }
}

void Steam_Overlay::steam_run_callback_friends_actions()
{
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    std::for_each(friends.begin(), friends.end(), [this](std::pair<Friend const, friend_window_state> &i) {
        i.second.joinable = is_friend_joinable(i);
    });

    while (!has_friend_action.empty()) {
        auto friend_info = friends.find(has_friend_action.front());
        if (friend_info != friends.end()) {
            uint64 friend_id = (uint64)friend_info->first.id();
            // The user clicked on "Send"
            if (friend_info->second.window_state & window_state_send_message) {
                char* input = friend_info->second.chat_input;
                char* end_input = input + strlen(input);
                char* printable_char = std::find_if(input, end_input, [](char c) { return std::isgraph(c); });

                // Check if the message contains something else than blanks
                if (printable_char != end_input) {
                    // Handle chat send
                    Common_Message msg;
                    Steam_Messages* steam_messages = new Steam_Messages;
                    steam_messages->set_type(Steam_Messages::FRIEND_CHAT);
                    steam_messages->set_message(friend_info->second.chat_input);
                    msg.set_allocated_steam_messages(steam_messages);
                    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
                    msg.set_dest_id(friend_id);
                    network->sendTo(&msg, true);

                    friend_info->second.chat_history.append(get_steam_client()->settings_client->get_local_name()).append(": ").append(input).append("\n", 1);
                }
                *input = 0; // Reset the input field

                friend_info->second.window_state &= ~window_state_send_message;
            }
            // The user clicked on "Invite" (but invite all wasn't clicked)
            if (friend_info->second.window_state & window_state_invite) {
                invite_friend(friend_id, steamFriends, steamMatchmaking);

                friend_info->second.window_state &= ~window_state_invite;
            }
            // The user clicked on "Join"
            if (friend_info->second.window_state & window_state_join) {
                std::string connect = steamFriends->get_friend_rich_presence_silent(friend_id, "connect");
                // The user got a lobby invite and accepted it
                if (friend_info->second.window_state & window_state_lobby_invite) {
                    GameLobbyJoinRequested_t data;
                    data.m_steamIDLobby.SetFromUint64(friend_info->second.lobbyId);
                    data.m_steamIDFriend.SetFromUint64(friend_id);
                    callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

                    friend_info->second.window_state &= ~window_state_lobby_invite;
                } else {
                    // The user got a rich presence invite and accepted it
                    if (friend_info->second.window_state & window_state_rich_invite) {
                        GameRichPresenceJoinRequested_t data = {};
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        strncpy(data.m_rgchConnect, friend_info->second.connect, k_cchMaxRichPresenceValueLength - 1);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

                        friend_info->second.window_state &= ~window_state_rich_invite;
                    } else if (connect.length() > 0) {
                        GameRichPresenceJoinRequested_t data = {};
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        strncpy(data.m_rgchConnect, connect.c_str(), k_cchMaxRichPresenceValueLength - 1);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }

                    //Not sure about this but it fixes sonic racing transformed invites
                    FriendGameInfo_t friend_game_info = {};
                    steamFriends->GetFriendGamePlayed(friend_id, &friend_game_info);
                    uint64 lobby_id = friend_game_info.m_steamIDLobby.ConvertToUint64();
                    if (lobby_id) {
                        GameLobbyJoinRequested_t data;
                        data.m_steamIDLobby.SetFromUint64(lobby_id);
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }
                }

                friend_info->second.window_state &= ~window_state_join;
            }
        }
        has_friend_action.pop();
    }

}

void Steam_Overlay::steam_run_callback()
{
    std::unique_lock lock(overlay_mutex, std::try_to_lock);
    if (!lock || !Ready()) return;

    if (!startup_ready_logged) {
        startup_ready_logged = true;
        log_overlay_startup("Overlay rendered its first frame.");
    }

    if (overlay_state_changed) {
        overlay_state_changed = false;

        GameOverlayActivated_t data{};
        data.m_bActive = show_overlay;
        data.m_bUserInitiated = true;
        data.m_dwOverlayPID = 123;
        data.m_nAppID = settings->get_local_game_id().AppID();
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    }

    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    if (save_settings.exchange(false)) {
        const char *language_text = valid_languages[current_language];
        save_global_settings(get_steam_client()->local_storage, username_text, language_text);
        get_steam_client()->settings_client->set_local_name(username_text);
        get_steam_client()->settings_server->set_local_name(username_text);
        get_steam_client()->settings_client->set_language(language_text);
        get_steam_client()->settings_server->set_language(language_text);
        steamFriends->resend_friend_data();
    }

    displayed_local_name = settings->get_local_name();
    displayed_app_id = settings->get_local_game_id().AppID();
    steam_run_callback_update_my_lobby();

    if (invite_all_friends_clicked.exchange(false)) {
        PRINT_DEBUG("Steam_Overlay will send invitations to [%zu] friends if they're using the same app", friends.size());
        uint32 current_appid = settings->get_local_game_id().AppID();
        const auto discovery = network->peer_discovery_snapshot();
        for (auto &fr : friends) {
            if (fr.first.appid() == current_appid && overlay_peer_visible(discovery, fr.first.id())) {
                uint64 friend_id = (uint64)fr.first.id();
                invite_friend(friend_id, steamFriends, steamMatchmaking);
            }
        }
    }

    steam_run_callback_friends_actions();
}



// -- steam networking callbacks --
void Steam_Overlay::networking_msg_received(Common_Message *msg)
{
    if (msg->has_steam_messages()) {
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

        Friend frd;
        frd.set_id(msg->source_id());
        auto friend_info = friends.find(frd);
        if (friend_info != friends.end()) {
            Steam_Messages const& steam_message = msg->steam_messages();
            // Change color to cyan for friend
            friend_info->second.chat_history.append(friend_info->first.name() + ": " + steam_message.message()).append("\n", 1);
            if (!(friend_info->second.window_state & window_state_show)) {
                friend_info->second.window_state |= window_state_need_attention;
            }

            add_chat_message_notification(friend_info->first.name() + ": " + steam_message.message());
            notify_sound_user_invite(friend_info->second);
        }
    }
}

// -- Screenshot capture callback --
void Steam_Overlay::on_screenshot_captured(const InGameOverlay::ScreenshotCallbackParameter_t* screenshot, void* userParameter)
{
    auto* self = static_cast<Steam_Overlay*>(userParameter);
    if (!screenshot || !screenshot->Data || screenshot->Width == 0 || screenshot->Height == 0)
        return;

    auto pixels = ScreenshotFormat::ConvertToRGBA(screenshot, 4);
    if (pixels.empty())
        return;

    CapturedScreenshot item;
    item.width = screenshot->Width;
    item.height = screenshot->Height;
    item.pixels_rgb = std::move(pixels);

    std::lock_guard<std::mutex> lock(self->captured_screenshots_mutex);
    self->captured_screenshots_queue.push_back(std::move(item));
}

void Steam_Overlay::process_captured_screenshots()
{
    std::vector<CapturedScreenshot> batch;
    {
        std::lock_guard<std::mutex> lock(captured_screenshots_mutex);
        if (captured_screenshots_queue.empty()) return;
        batch.swap(captured_screenshots_queue);
    }

    for (auto& item : batch) {
        char buff[128];
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        struct tm local_tm{};
#ifdef _MSC_VER
        localtime_s(&local_tm, &now_time);
#else
        localtime_r(&now_time, &local_tm);
#endif
        size_t written = std::strftime(buff, sizeof(buff), settings->overlay_appearance.screenshot_datetime_format.c_str(), &local_tm);
        if (!written) {
            std::strftime(buff, sizeof(buff), "%Y/%m/%d - %H:%M:%S", &local_tm);
        }
        std::string filename = buff;
        for (char& c : filename) {
            if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '.';
            }
        }
        filename += ".png";

        if (local_storage->save_screenshot(filename, item.pixels_rgb.data(), item.width, item.height, 4)) {
            PRINT_DEBUG("Screenshot saved: %s", filename.c_str());
            submit_notification(notification_type::screenshot, translationScreenshotSaved[current_language] + filename);
        } else {
            PRINT_DEBUG("Failed to save screenshot!");
        }
    }

    refresh_screenshots_list();
}

// -- Screenshots directory scanning --
void Steam_Overlay::refresh_screenshots_list()
{
    // Unload existing textures
    for (auto& item : screenshot_items) {
        if (item.texture) {
            if (item.texture->GetResourceId() != 0)
                item.texture->Unload();
            item.texture->Delete();
        }
    }
    screenshot_items.clear();

    if (!local_storage->dir_exists(Local_Storage::screenshots_folder)) {
        return;
    }

    std::string path = local_storage->get_path(Local_Storage::screenshots_folder);
    auto filenames = Local_Storage::get_filenames_path(path);

    for (auto& f : filenames) {
        if (f.size() < 4) continue;
        std::string ext = f.substr(f.size() - 4);
        if (ext != ".png" && ext != ".PNG") continue;

        ScreenshotItem item;
        item.filename = f;
        item.full_path = path + PATH_SEPARATOR + f;
        // Read file modification time
#ifdef __WINDOWS__
        struct _stat st;
        auto wstat_path = common_helpers::to_wstr(item.full_path);
        if (_wstat(wstat_path.c_str(), &st) == 0)
            item.mtime = st.st_mtime;
#else
        struct stat st;
        if (stat(item.full_path.c_str(), &st) == 0)
            item.mtime = st.st_mtime;
#endif
        if (_renderer)
            item.texture = _renderer->CreateResource();
        screenshot_items.push_back(std::move(item));
    }

    // Sort oldest first so the gallery shows screenshots in chronological order
    std::sort(screenshot_items.begin(), screenshot_items.end(),
              [](const ScreenshotItem& a, const ScreenshotItem& b) {
                  return a.mtime < b.mtime;
              });

    screenshots_loaded = true;
}

// -- Preview popup state cleanup --
// Releases the preview's GPU texture and resets all state flags. Callers are responsible
// for calling ImGui::CloseCurrentPopup() (or doing so in the same scope) if the popup is
// currently open — this helper only tears down the C++ state.
void Steam_Overlay::clear_preview_state()
{
    preview_screenshot_path.clear();
    preview_open_active = false;
    preview_delete_pending = false;
    preview_crop_mode = false;
    preview_crop_rect = ImVec4(0, 0, 0, 0);
    preview_crop_rect_prev = ImVec4(0, 0, 0, 0);
    preview_crop_drag = CropDragState{};
    preview_index = -1;
    if (preview_texture) {
        if (preview_texture->GetResourceId() != 0)
            preview_texture->Unload();
        preview_texture->Delete();
        preview_texture = nullptr;
    }
    preview_pixels.clear();
    preview_pixels_w = 0;
    preview_pixels_h = 0;
}

// -- Gallery window --
void Steam_Overlay::render_gallery_window()
{
    if (!show_screenshots_window) return;

    ImGui::PushFont(font_default);
    uint32 style_color_stack = apply_global_style_color();

    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(8192, 8192));
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin(translationScreenshots[current_language], &show_screenshots_window)) {
        // Delete/Unpin toolbar
        bool has_selection = false;
        for (auto& item : screenshot_items) {
            if (item.selected) { has_selection = true; break; }
        }

        if (!pinned_screenshots.empty()) {
            if (ImGui::Button(translationUnpinAll[current_language])) {
                unpin_all_screenshots();
            }
            ImGui::SameLine();
        }

        if (has_selection) {
            if (ImGui::Button(translationDeleteSelected[current_language])) {
                delete_all_selected = true;
                show_delete_confirmation = true;
                delete_confirm_open_active = true;
            }
            ImGui::SameLine();
        }

        if (!screenshot_items.empty()) {
            if (ImGui::Button(translationOpenFolder[current_language])) {
                std::string path = local_storage->get_path(Local_Storage::screenshots_folder);
#ifdef __WINDOWS__
                auto wpath = common_helpers::to_wstr(path);
                ShellExecuteW(NULL, L"open", wpath.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__linux__)
                std::string cmd = "xdg-open \"" + path + "\"";
                std::system(cmd.c_str());
#elif defined(__APPLE__)
                std::string cmd = "open \"" + path + "\"";
                std::system(cmd.c_str());
#endif
            }
        }

        ImGui::Separator();

        if (screenshot_items.empty()) {
            if (!screenshots_loaded)
                refresh_screenshots_list();
            if (screenshot_items.empty()) {
                ImGui::TextDisabled(translationNoScreenshotsYet[current_language]);
            }
        }

        // Thumbnail grid — use a fixed-column-count table so items don't
        // slide around when the window is resized.  Each column is sized
        // to exactly one thumbnail + checkbox + small gap.
        if (!screenshot_items.empty()) {
            const float thumb_width  = 160.0f;
            const float thumb_height = 90.0f;
            const float check_w = ImGui::GetFrameHeight()
                                  + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float cell_w = thumb_width + check_w
                                 + ImGui::GetStyle().ItemSpacing.x;

            float avail_x = ImGui::GetContentRegionAvail().x;
            int columns = std::max(1, (int)(avail_x / cell_w));

            if (ImGui::BeginTable("##screenshot_grid", columns,
                    ImGuiTableFlags_SizingFixedFit)) {
                // Each column gets the same fixed width so the grid doesn't
                // reflow on tiny width changes.
                for (int c = 0; c < columns; ++c) {
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cell_w);
                }
                for (int idx = 0; idx < (int)screenshot_items.size(); ++idx) {
                    ImGui::TableNextColumn();

                    auto& item = screenshot_items[idx];
                    ImGui::PushID(item.filename.c_str());

                // Load thumbnail texture lazily
                if (item.texture && item.texture->GetResourceId() == 0 && !item.failed_to_load) {
                    int img_w = 0, img_h = 0;
                    unsigned char* img = stbi_load(item.full_path.c_str(), &img_w, &img_h, nullptr, 4);
                    if (img) {
                        item.thumbnail_pixels.assign((size_t)thumb_width * (size_t)thumb_height * 4, 0);
                        stbir_resize_uint8_linear(img, img_w, img_h, 0,
                            item.thumbnail_pixels.data(), (int)thumb_width, (int)thumb_height, 0, STBIR_RGBA);
                        item.texture->AttachResource(item.thumbnail_pixels.data(), (uint32_t)thumb_width, (uint32_t)thumb_height);
                        stbi_image_free(img);
                    } else {
                        item.failed_to_load = true;
                    }
                }

                // Thumbnail image button
                if (item.texture && item.texture->GetResourceId() != 0) {
                    if (ImGui::ImageButton("##thumb", item.texture->GetResourceId(), ImVec2(thumb_width, thumb_height))) {
                        preview_index = idx;
                        preview_screenshot_path = item.full_path;
                    }
                } else {
                    ImGui::InvisibleButton("##thumb_placeholder", ImVec2(thumb_width, thumb_height));
                }

                // Right-click context menu
                if (ImGui::BeginPopupContextItem("##screenshot_ctx")) {
                    if (ImGui::Selectable(translationPin[current_language])) {
                        PinnedScreenshot pin;
                        pin.id = next_pin_id++;
                        pin.path = item.full_path;

                        int img_w = 0, img_h = 0;
                        unsigned char* img = stbi_load(item.full_path.c_str(), &img_w, &img_h, nullptr, 4);
                        if (img) {
                            pin.pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                            pin.pixels_w = (uint32_t)img_w;
                            pin.pixels_h = (uint32_t)img_h;
                            pin.size = ImVec2((float)img_w, (float)img_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float scale = std::min(kContextPinMaxDim / pin.size.x,
                                                       kContextPinMaxDim / pin.size.y);
                                pin.size.x *= scale;
                                pin.size.y *= scale;
                            }
                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                pin.texture->AttachResource(pin.pixels.data(), pin.pixels_w, pin.pixels_h);
                            }
                            stbi_image_free(img);
                        }
                        pin.focus_requested = true;
                        pinned_screenshots.push_back(std::move(pin));
                    }
                    if (ImGui::Selectable(translationDelete[current_language])) {
                        single_delete_path = item.full_path;
                        delete_all_selected = false;
                        show_delete_confirmation = true;
                        delete_confirm_open_active = true;
                    }
                    ImGui::EndPopup();
                }

                // Checkbox with text beside it, width limited to thumbnail width
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                ImGui::Checkbox("##sel", &item.selected);
                ImGui::PopStyleVar();
                ImGui::SameLine(0, 0);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb_width);
                // Show filename (without .png extension) below thumbnail
                {
                    std::string label = item.filename;
                    if (label.size() > 4)
                        label.resize(label.size() - 4);
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::PopTextWrapPos();

                ImGui::PopID();
            }
            ImGui::EndTable();
            }
        }

        // -- Preview popup --
        if (!preview_screenshot_path.empty() || preview_open_active) {
            // Open the popup on the first frame only (not while navigating via Prev/Next)
            if (!preview_screenshot_path.empty() && !preview_open_active) {
                ImGui::OpenPopup(translationScreenshotPreview[current_language]);
                preview_open_active = true;

                // Find the index for Prev/Next navigation
                if (preview_index < 0 || preview_index >= (int)screenshot_items.size() ||
                    screenshot_items[preview_index].full_path != preview_screenshot_path)
                {
                    preview_index = -1;
                    for (int i = 0; i < (int)screenshot_items.size(); ++i) {
                        if (screenshot_items[i].full_path == preview_screenshot_path) {
                            preview_index = i;
                            break;
                        }
                    }
                }
                // Set initial window size proportional to display
                ImVec2 disp = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowSize(ImVec2(disp.x * 0.8f, disp.y * 0.8f), ImGuiCond_Appearing);
            }
            // Removed AlwaysAutoResize so the user can resize the preview window.
            // Center the popup on screen so it never appears at a weird position.
            ImVec2 disp_center = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(disp_center.x * 0.5f, disp_center.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGuiWindowFlags preview_flags = ImGuiWindowFlags_NoScrollbar;
            if (preview_crop_mode) {
                preview_flags |= ImGuiWindowFlags_NoMove;
            }
            bool preview_modal_open = true;
            if (ImGui::BeginPopupModal(translationScreenshotPreview[current_language], &preview_modal_open, preview_flags)) {
                // Navigate to a different screenshot
                // (path changed via Prev/Next → unload current texture so it reloads next frame)
                if (preview_texture && preview_index >= 0 && preview_index < (int)screenshot_items.size() &&
                    screenshot_items[preview_index].full_path != preview_screenshot_path)
                {
                    preview_screenshot_path = screenshot_items[preview_index].full_path;
                    // Reset crop state — old selection no longer applies to new image
                    preview_crop_mode = false;
                    preview_crop_rect = ImVec4(0, 0, 0, 0);
                    preview_crop_rect_prev = ImVec4(0, 0, 0, 0);
                    preview_crop_drag = CropDragState{};
                    if (preview_texture) {
                        if (preview_texture->GetResourceId() != 0)
                            preview_texture->Unload();
                        preview_texture->Delete();
                        preview_texture = nullptr;
                    }
                    preview_pixels.clear();
                    preview_pixels_w = 0;
                    preview_pixels_h = 0;
                }

                // Load full-resolution preview texture (not downscaled — scaling is done
                // at render time so the image fills the user-resizable window)
                if (preview_texture == nullptr) {
                    preview_texture = _renderer ? _renderer->CreateResource() : nullptr;
                }
                if (preview_texture && preview_texture->GetResourceId() == 0 &&
                    preview_index >= 0 && preview_index < (int)screenshot_items.size()) {
                    int img_w = 0, img_h = 0;
                    unsigned char* img = stbi_load(screenshot_items[preview_index].full_path.c_str(), &img_w, &img_h, nullptr, 4);
                    if (img) {
                        preview_pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                        preview_pixels_w = (uint32_t)img_w;
                        preview_pixels_h = (uint32_t)img_h;
                        preview_texture->AttachResource(preview_pixels.data(), preview_pixels_w, preview_pixels_h);
                        stbi_image_free(img);
                    }
                }

                // Display image scaled to fit available content, maintaining aspect ratio.
                bool preview_texture_ok = preview_texture && preview_texture->GetResourceId() != 0 && preview_pixels_w > 0 && preview_pixels_h > 0;
                if (preview_texture_ok) {
                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    // Reserve space for the date + button bar at the bottom (only when not in crop mode)
                    if (!preview_crop_mode) {
                        float btn_h = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 4.0f;
                        avail.y -= btn_h;
                    }
                    float scale = std::min(avail.x / (float)preview_pixels_w, avail.y / (float)preview_pixels_h);
                    ImVec2 preview_display_size = ImVec2((float)preview_pixels_w * scale, (float)preview_pixels_h * scale);
                    // Center the image horizontally so portrait images don't leave a gap on the right
                    float off_x = (avail.x - preview_display_size.x) * 0.5f;
                    if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);
                    ImVec2 preview_img_pos = ImGui::GetCursorScreenPos();
                    ImGui::Image(preview_texture->GetResourceId(), preview_display_size);

                    // If in crop mode, draw the crop editor over the image. The
                    // Confirm action creates a new pin with preview_crop_rect.
                    // Cancel exits crop mode and restores the previous rect.
                    if (preview_crop_mode) {
                        // (0,0,0,0) = no selection — user will click-and-drag.
                        // Non-zero degenerate rect: clamp to bounds as safety net.
                        if (preview_crop_rect.z <= preview_crop_rect.x
                            || preview_crop_rect.w <= preview_crop_rect.y) {
                            if (preview_crop_rect.x != 0 || preview_crop_rect.y != 0 ||
                                preview_crop_rect.z != 0 || preview_crop_rect.w != 0) {
                                preview_crop_rect.x = std::max(0.0f, std::min((float)preview_pixels_w, preview_crop_rect.x));
                                preview_crop_rect.y = std::max(0.0f, std::min((float)preview_pixels_h, preview_crop_rect.y));
                                preview_crop_rect.z = std::max(preview_crop_rect.x, std::min((float)preview_pixels_w, preview_crop_rect.z));
                                preview_crop_rect.w = std::max(preview_crop_rect.y, std::min((float)preview_pixels_h, preview_crop_rect.w));
                            }
                            // Zero rect (0,0,0,0) left as-is — means "no selection"
                        }
                        CropAction act = render_crop_editor(preview_crop_rect,
                            preview_crop_drag,
                            preview_texture->GetResourceId(),
                            preview_pixels_w, preview_pixels_h,
                            preview_img_pos, preview_display_size);
                        if (act == CropAction::Confirm) {
                            // Create a new pin with the selected crop region
                            PinnedScreenshot pin;
                            pin.id = next_pin_id++;
                            pin.path = screenshot_items[preview_index].full_path;
                            if (preview_pixels_w > 0 && preview_pixels_h > 0) {
                                pin.pixels = preview_pixels;
                                pin.pixels_w = preview_pixels_w;
                                pin.pixels_h = preview_pixels_h;
                            }
                            // Initial size = the crop region (not the full image)
                            float crop_w = std::max(1.0f, preview_crop_rect.z - preview_crop_rect.x);
                            float crop_h = std::max(1.0f, preview_crop_rect.w - preview_crop_rect.y);
                            pin.size = ImVec2(crop_w, crop_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float ss = std::min(kContextPinMaxDim / pin.size.x,
                                                    kContextPinMaxDim / pin.size.y);
                                pin.size.x *= ss;
                                pin.size.y *= ss;
                            }
                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                if (pin.texture && !pin.pixels.empty())
                                    pin.texture->AttachResource(pin.pixels.data(),
                                        pin.pixels_w, pin.pixels_h);
                            }
                            pin.crop_rect = preview_crop_rect;
                            pin.focus_requested = true;
                            pinned_screenshots.push_back(std::move(pin));

                            // Exit crop mode and close the preview
                            preview_crop_mode = false;
                            ImGui::CloseCurrentPopup();
                            clear_preview_state();
                        } else if (act == CropAction::Cancel) {
                            preview_crop_rect = preview_crop_rect_prev;
                            preview_crop_mode = false;
                        }
                    } else {
                        ImGui::Separator();

                        // Screenshot date — only shown when texture is loaded (avoids flash on navigation)
                        if (!screenshot_items.empty() && preview_index >= 0 && preview_index < (int)screenshot_items.size()) {
                            auto& src_item = screenshot_items[preview_index];
                            if (src_item.mtime > 0) {
                                char time_buf[64];
                                struct tm local_tm{};
#ifdef _MSC_VER
                                localtime_s(&local_tm, &src_item.mtime);
#else
                                localtime_r(&src_item.mtime, &local_tm);
#endif
                                size_t written = std::strftime(time_buf, sizeof(time_buf), settings->overlay_appearance.screenshot_datetime_format.c_str(), &local_tm);
                                if (!written) {
                                    std::strftime(time_buf, sizeof(time_buf), "%Y/%m/%d - %H:%M:%S", &local_tm);
                                }
                                ImGui::TextUnformatted(time_buf);
                            } else {
                                ImGui::TextUnformatted(src_item.filename.c_str());
                            }
                        }

                        // Button bar: < Prev | Pin  Crop  Delete | Next >
                        ImGui::BeginGroup();

                        // Prev — always visible, wraps to last
                        if (ImGui::Button(translationPrev[current_language])) {
                            if (preview_index <= 0)
                                preview_index = (int)screenshot_items.size() - 1;
                            else
                                preview_index--;
                        }
                        ImGui::SameLine();

                        ImGui::Text("|");
                        ImGui::SameLine();

                        if (ImGui::Button(translationPin[current_language])) {
                            PinnedScreenshot pin;
                            pin.id = next_pin_id++;
                            pin.path = screenshot_items[preview_index].full_path;

                            // Repurpose the already-loaded preview pixels to avoid a second stbi_load
                            if (preview_pixels_w > 0 && preview_pixels_h > 0) {
                                pin.pixels = preview_pixels; // copies — small enough for a single frame
                                pin.pixels_w = preview_pixels_w;
                                pin.pixels_h = preview_pixels_h;
                            } else {
                                int img_w = 0, img_h = 0;
                                unsigned char* img = stbi_load(pin.path.c_str(), &img_w, &img_h, nullptr, 4);
                                if (img) {
                                    pin.pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                                    pin.pixels_w = (uint32_t)img_w;
                                    pin.pixels_h = (uint32_t)img_h;
                                    stbi_image_free(img);
                                }
                            }

                            // Same initial sizing as context-menu pin (kContextPinMaxDim)
                            pin.size = ImVec2((float)pin.pixels_w, (float)pin.pixels_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float scale = std::min(kContextPinMaxDim / pin.size.x,
                                                       kContextPinMaxDim / pin.size.y);
                                pin.size.x *= scale;
                                pin.size.y *= scale;
                            }

                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                if (pin.texture && !pin.pixels.empty())
                                    pin.texture->AttachResource(pin.pixels.data(), pin.pixels_w, pin.pixels_h);
                            }
                            pin.focus_requested = true;
                            pinned_screenshots.push_back(std::move(pin));

                            // Leave preview open — pin is immediately visible in its own window
                        }
                        ImGui::SameLine();

                        if (ImGui::Button(translationCrop[current_language])) {
                            preview_crop_rect_prev = preview_crop_rect;
                            preview_crop_mode = true;
                        }
                        ImGui::SameLine();

                        if (!preview_delete_pending && ImGui::Button(translationDelete[current_language])) {
                            preview_delete_pending = true;
                        }
                        ImGui::SameLine();

                        ImGui::Text("|");
                        ImGui::SameLine();

                        // Next — always visible, wraps to first
                        if (ImGui::Button(translationNext[current_language])) {
                            if (preview_index >= (int)screenshot_items.size() - 1)
                                preview_index = 0;
                            else
                                preview_index++;
                        }

                        ImGui::EndGroup();
                    }

                    // Inline delete confirmation (avoids stacking modals which closes the preview)
                    if (preview_delete_pending) {
                        ImGui::Separator();
                        ImGui::Text(translationDeleteThisScreenshot[current_language]);
                        ImGui::SameLine();
                        if (ImGui::Button(translationYes[current_language])) {
                            preview_delete_pending = false;
                            // Perform the delete inline
                            auto& del_item = screenshot_items[preview_index];
                            std::string base_path = local_storage->get_path(Local_Storage::screenshots_folder) + PATH_SEPARATOR;
                            std::string filename;
                            if (del_item.full_path.find(base_path) == 0) {
                                filename = del_item.full_path.substr(base_path.size());
                            } else {
                                auto pos = del_item.full_path.find_last_of("/\\");
                                filename = (pos != std::string::npos) ? del_item.full_path.substr(pos + 1) : del_item.full_path;
                            }
                            if (!filename.empty()) {
                                local_storage->file_delete(Local_Storage::screenshots_folder, filename);
                                std::string json_name = filename.substr(0, filename.size() - 4) + ".json";
                                local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                            }
                            // Remove pin if the deleted file was pinned
                            for (auto pit = pinned_screenshots.begin(); pit != pinned_screenshots.end(); ) {
                                if (pit->path == del_item.full_path) {
                                    if (pit->texture) {
                                        if (pit->texture->GetResourceId() != 0)
                                            pit->texture->Unload();
                                        pit->texture->Delete();
                                    }
                                    pit = pinned_screenshots.erase(pit);
                                } else {
                                    ++pit;
                                }
                            }
                            // Refresh and advance to next
                            refresh_screenshots_list();
                            if (screenshot_items.empty()) {
                                ImGui::CloseCurrentPopup();
                                clear_preview_state();
                            } else {
                                if (preview_index >= (int)screenshot_items.size())
                                    preview_index = (int)screenshot_items.size() - 1;
                                preview_screenshot_path = screenshot_items[preview_index].full_path;
                                if (preview_texture) {
                                    if (preview_texture->GetResourceId() != 0)
                                        preview_texture->Unload();
                                    preview_texture->Delete();
                                    preview_texture = nullptr;
                                }
                                preview_pixels.clear();
                                preview_pixels_w = 0;
                                preview_pixels_h = 0;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(translationNo[current_language])) {
                            preview_delete_pending = false;
                        }
                    }
                }

                ImGui::EndPopup();
            } else {
                // BeginPopupModal returned false. Only clear state if the popup is
                // truly closed (X/Escape), not just covered by another window like a new pin.
                if (preview_open_active && !ImGui::IsPopupOpen(translationScreenshotPreview[current_language])) {
                    clear_preview_state();
                }
            }
            // X button on modal title bar → ImGui sets preview_modal_open to false
            if (!preview_modal_open && preview_open_active) {
                clear_preview_state();
            }
        }

        // -- Delete confirmation modal --
        if (show_delete_confirmation || delete_confirm_open_active) {
            if (show_delete_confirmation) {
                ImGui::OpenPopup(translationConfirmDelete[current_language]);
                delete_confirm_open_active = true;
                show_delete_confirmation = false; // consumed - the active flag carries the state
            }
            if (ImGui::BeginPopupModal(translationConfirmDelete[current_language], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (delete_all_selected) {
                    ImGui::Text(translationDeleteAllScelectedScreenshots[current_language]);
                } else {
                    ImGui::Text(translationDeleteThisScreenshot[current_language]);
                }
                ImGui::Separator();
                if (ImGui::Button(translationYes[current_language])) {
                    if (delete_all_selected) {
                        // Delete all selected
                        for (auto& item : screenshot_items) {
                            if (!item.selected) continue;
                            local_storage->file_delete(Local_Storage::screenshots_folder, item.filename);
                            // Also delete the .json metadata if it exists
                            std::string json_name = item.filename.substr(0, item.filename.size() - 4) + ".json";
                            local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                        }
                        refresh_screenshots_list();
                        // Remove pins for files that no longer exist
                        for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
                            bool exists = false;
                            for (auto& si : screenshot_items) {
                                if (si.full_path == it->path) { exists = true; break; }
                            }
                            if (!exists) {
                                if (it->texture) {
                                    if (it->texture->GetResourceId() != 0)
                                        it->texture->Unload();
                                    it->texture->Delete();
                                }
                                it = pinned_screenshots.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    } else if (!single_delete_path.empty()) {
                        // Find filename from path
                        std::string path = local_storage->get_path(Local_Storage::screenshots_folder) + PATH_SEPARATOR;
                        std::string filename;
                        if (single_delete_path.find(path) == 0) {
                            filename = single_delete_path.substr(path.size());
                        } else {
                            // Try just the basename
                            auto pos = single_delete_path.find_last_of("/\\");
                            filename = (pos != std::string::npos) ? single_delete_path.substr(pos + 1) : single_delete_path;
                        }
                        if (!filename.empty()) {
                            local_storage->file_delete(Local_Storage::screenshots_folder, filename);
                            std::string json_name = filename.substr(0, filename.size() - 4) + ".json";
                            local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                        }
                        refresh_screenshots_list();
                        // Remove pin if the deleted file was pinned
                        for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
                            if (it->path == single_delete_path) {
                                if (it->texture) {
                                    if (it->texture->GetResourceId() != 0)
                                        it->texture->Unload();
                                    it->texture->Delete();
                                }
                                it = pinned_screenshots.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    single_delete_path.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(translationNo[current_language])) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            } else {
                if (delete_confirm_open_active) {
                    // User closed it (Yes or No handled inside the modal). Clear state.
                    show_delete_confirmation = false;
                    single_delete_path.clear();
                    delete_all_selected = false;
                    delete_confirm_open_active = false;
                }
            }
        }
    }
    ImGui::End();

    if (style_color_stack) ImGui::PopStyleColor(style_color_stack);
    ImGui::PopFont();
}

// -- Pin helpers --
void Steam_Overlay::unpin_screenshot(uint64_t id)
{
    for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ++it) {
        if (it->id == id) {
            if (it->texture) {
                if (it->texture->GetResourceId() != 0)
                    it->texture->Unload();
                it->texture->Delete();
            }
            pinned_screenshots.erase(it);
            return;
        }
    }
}

void Steam_Overlay::unpin_all_screenshots()
{
    for (auto& pin : pinned_screenshots) {
        if (pin.texture) {
            if (pin.texture->GetResourceId() != 0)
                pin.texture->Unload();
            pin.texture->Delete();
        }
    }
    pinned_screenshots.clear();
}

// -- Floating pinned screenshots --

// Render the crop-rectangle editor overlay. The caller renders the FULL
// image with UV (0,0)-(1,1) at img_min/img_size before calling this. This
// function draws:
//   1) a dim layer over the whole image,
//   2) the image AGAIN, clipped to the selection rect, so the selection
//      region looks "un-dimmed" while everything else is darkened,
//   3) a bright border and 8 drag handles,
//   4) a small toolbar with Confirm and Cancel buttons.
// `tex_id` is the texture's GPU resource ID used for step 2.
// `rect` is in source-pixel coordinates and is mutated in place. Returns
// Active until the user clicks Confirm or Cancel.
Steam_Overlay::CropAction Steam_Overlay::render_crop_editor(
    ImVec4& rect, CropDragState& st,
    ImTextureID tex_id,
    uint32_t src_w, uint32_t src_h,
    ImVec2 img_min, ImVec2 img_size)
{
    constexpr float kMinCropPx = 20.0f;
    constexpr float kHandlePx = 8.0f;
    constexpr float kHandleHit = 10.0f;     // generous hit area for handles

    if (src_w == 0 || src_h == 0 || img_size.x <= 0 || img_size.y <= 0)
        return CropAction::Active;

    // Helper: convert source pixel coords -> screen rect
    auto src_to_screen = [&](ImVec4 r) -> ImVec4 {
        float sx = img_size.x / (float)src_w;
        float sy = img_size.y / (float)src_h;
        return ImVec4(img_min.x + r.x * sx,
                      img_min.y + r.y * sy,
                      img_min.x + r.z * sx,
                      img_min.y + r.w * sy);
    };
    // Helper: convert screen point -> source pixel coords
    auto screen_to_src = [&](ImVec2 p) -> ImVec2 {
        float sx = (float)src_w / img_size.x;
        float sy = (float)src_h / img_size.y;
        return ImVec2((p.x - img_min.x) * sx,
                      (p.y - img_min.y) * sy);
    };
    // Helper: clamp rect to image bounds
    auto clamp_rect = [&](ImVec4 r) -> ImVec4 {
        // When clamping x/y to 0, shift z/w by the same delta so the rect
        // keeps its original width/height instead of collapsing to the
        // minimum size.  This prevents a visible "collapse" when the user
        // body-drags the selection off the left or top edge.
        if (r.x < 0) {
            r.z -= r.x;     // r.x is negative, so this adds |r.x| to r.z
            r.x = 0;
        }
        if (r.y < 0) {
            r.w -= r.y;     // r.y is negative, so this adds |r.y| to r.w
            r.y = 0;
        }
        if (r.z > (float)src_w) r.z = (float)src_w;
        if (r.w > (float)src_h) r.w = (float)src_h;
        // Enforce minimum size
        if (r.z - r.x < kMinCropPx) {
            float c = (r.x + r.z) * 0.5f;
            r.x = std::max(0.0f, c - kMinCropPx * 0.5f);
            r.z = std::min((float)src_w, r.x + kMinCropPx);
        }
        if (r.w - r.y < kMinCropPx) {
            float c = (r.y + r.w) * 0.5f;
            r.y = std::max(0.0f, c - kMinCropPx * 0.5f);
            r.w = std::min((float)src_h, r.y + kMinCropPx);
        }
        return r;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1) Dim overlay over the whole image
    dl->AddRectFilled(img_min,
                      ImVec2(img_min.x + img_size.x, img_min.y + img_size.y),
                      IM_COL32(0, 0, 0, 140));

    // 2) Selection rect (clamped, drawn in screen coords)
    rect = clamp_rect(rect);
    ImVec4 sel_screen = src_to_screen(rect);
    ImVec2 s0(sel_screen.x, sel_screen.y);
    ImVec2 s1(sel_screen.z, sel_screen.w);

    // Redraw the image in the selection area so the dim layer doesn't
    // cover it. UV is mapped from the source's crop region.
    if (tex_id) {
        ImVec2 uv0(rect.x / (float)src_w, rect.y / (float)src_h);
        ImVec2 uv1(rect.z / (float)src_w, rect.w / (float)src_h);
        dl->AddImage(tex_id, s0, s1, uv0, uv1,
                     IM_COL32(255, 255, 255, 255));
    } else {
        // Fallback: just a translucent white wash so the user can see the
        // selection area is "different" from the dim surroundings.
        dl->AddRectFilled(s0, s1, IM_COL32(255, 255, 255, 30));
    }
    // Border
    dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);

    // 3) 8 handles (4 corners + 4 edge midpoints)
    ImVec2 corners[8] = {
        ImVec2(s0.x, s0.y),                 // 0: TL
        ImVec2(s1.x, s0.y),                 // 1: TR
        ImVec2(s1.x, s1.y),                 // 2: BR
        ImVec2(s0.x, s1.y),                 // 3: BL
        ImVec2((s0.x + s1.x) * 0.5f, s0.y), // 4: T
        ImVec2(s1.x, (s0.y + s1.y) * 0.5f), // 5: R
        ImVec2((s0.x + s1.x) * 0.5f, s1.y), // 6: B
        ImVec2(s0.x, (s0.y + s1.y) * 0.5f), // 7: L
    };
    for (int i = 0; i < 8; ++i) {
        dl->AddRectFilled(
            ImVec2(corners[i].x - kHandlePx * 0.5f, corners[i].y - kHandlePx * 0.5f),
            ImVec2(corners[i].x + kHandlePx * 0.5f, corners[i].y + kHandlePx * 0.5f),
            IM_COL32(255, 255, 255, 255));
        dl->AddRect(
            ImVec2(corners[i].x - kHandlePx * 0.5f, corners[i].y - kHandlePx * 0.5f),
            ImVec2(corners[i].x + kHandlePx * 0.5f, corners[i].y + kHandlePx * 0.5f),
            IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
    }

    // 4) Toolbar (Confirm/Cancel) — rendered directly in the parent window
    //    (no separate child window) to avoid input-routing issues where
    //    clicks on the image get intercepted by the parent window's title
    //    bar or the toolbar steals focus.  The background is drawn first
    //    (via the draw list) so the buttons render on top of it.
    const float tb_fp_x = 6.0f, tb_fp_y = 4.0f;
    const float tb_gap = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 confirm_sz = ImGui::CalcTextSize(translationConfirm[current_language]);
    const ImVec2 cancel_sz  = ImGui::CalcTextSize(translationCancel[current_language]);
    const float tb_w = (confirm_sz.x + tb_fp_x * 2) + tb_gap
                     + (cancel_sz.x  + tb_fp_x * 2);
    const float tb_h = std::max(confirm_sz.y, cancel_sz.y) + tb_fp_y * 2;
    const ImVec2 tb_tl = img_min;
    const ImVec2 tb_br(tb_tl.x + tb_w, tb_tl.y + tb_h);

    // Draw background rounded rect BEFORE buttons so they stack on top
    dl->AddRectFilled(tb_tl, tb_br, IM_COL32(0, 0, 0, 178), 4.0f);

    // Render buttons into the parent window at the toolbar position
    ImGui::SetCursorScreenPos(tb_tl);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(tb_fp_x, tb_fp_y));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));

    CropAction action = CropAction::Active;
    if (ImGui::Button(translationConfirm[current_language])) {
        action = CropAction::Confirm;
    }
    ImGui::SameLine();
    if (ImGui::Button(translationCancel[current_language])) {
        action = CropAction::Cancel;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImVec2 toolbar_min = tb_tl;
    ImVec2 toolbar_max = tb_br;

    // 5) Mouse input — skip clicks that fall inside the toolbar so the
    //    Confirm/Cancel buttons aren't treated as crop drags.
    ImVec2 mouse = ImGui::GetMousePos();
    bool mouse_in_toolbar = mouse.x >= toolbar_min.x && mouse.x <= toolbar_max.x
                         && mouse.y >= toolbar_min.y && mouse.y <= toolbar_max.y;
    bool mouse_in_image = !mouse_in_toolbar
                       && mouse.x >= img_min.x && mouse.x <= img_min.x + img_size.x
                       && mouse.y >= img_min.y && mouse.y <= img_min.y + img_size.y;
    bool mouse_down = ImGui::IsMouseDown(0);
    bool mouse_clicked = ImGui::IsMouseClicked(0);

    // Determine which handle (if any) the mouse is over
    int hit_handle = -1;
    if (mouse_in_image) {
        for (int i = 0; i < 8; ++i) {
            if (fabsf(mouse.x - corners[i].x) <= kHandleHit * 0.5f
             && fabsf(mouse.y - corners[i].y) <= kHandleHit * 0.5f) {
                hit_handle = i;
                break;
            }
        }
    }
    st.handle_hover = hit_handle;

    // Cursor feedback based on what's being hovered
    if (st.dragging) {
        if (st.handle == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (st.handle >= 0 && st.handle <= 3) {
            // Diagonal resize cursors
            int diag = (st.handle == 0 || st.handle == 2) ? 0 : 1;
            ImGui::SetMouseCursor(diag == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        } else if (st.handle >= 4 && st.handle <= 7) {
            ImGui::SetMouseCursor((st.handle == 4 || st.handle == 6)
                ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        }
    } else if (hit_handle >= 0) {
        if (hit_handle <= 3) {
            int diag = (hit_handle == 0 || hit_handle == 2) ? 0 : 1;
            ImGui::SetMouseCursor(diag == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        } else if (hit_handle >= 4 && hit_handle <= 7) {
            ImGui::SetMouseCursor((hit_handle == 4 || hit_handle == 6)
                ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        }
    } else if (mouse_in_image && mouse.x >= s0.x && mouse.x <= s1.x
            && mouse.y >= s0.y && mouse.y <= s1.y) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (!st.dragging) {
        if (mouse_clicked && mouse_in_image) {
            if (hit_handle >= 0) {
                // Start handle drag
                st.dragging = true;
                st.handle = hit_handle;
                st.start = mouse;
                // Anchor: for corners, opposite corner. For edges, we
                // preserve the opposite edge automatically by only mutating
                // the dragged edge in the drag code below, so anchor isn't
                // needed for edges.
                if (hit_handle == 0) st.anchor = ImVec2(rect.z, rect.w);
                else if (hit_handle == 1) st.anchor = ImVec2(rect.x, rect.w);
                else if (hit_handle == 2) st.anchor = ImVec2(rect.x, rect.y);
                else if (hit_handle == 3) st.anchor = ImVec2(rect.z, rect.y);
            } else if (mouse.x >= s0.x && mouse.x <= s1.x
                    && mouse.y >= s0.y && mouse.y <= s1.y) {
                // Start body move
                st.dragging = true;
                st.handle = 8;
                ImVec2 src_mouse = screen_to_src(mouse);
                st.anchor = ImVec2(src_mouse.x - rect.x, src_mouse.y - rect.y);
            } else {
                // Click outside selection: start a fresh draw anchored at the
                // click. The user drags to define the area.
                ImVec2 src_mouse = screen_to_src(mouse);
                st.dragging = true;
                st.handle = 9;  // 9 = initial-draw mode
                rect = ImVec4(src_mouse.x, src_mouse.y, src_mouse.x, src_mouse.y);
                st.anchor = src_mouse;
            }
        }
    } else {
        // In-progress drag
        if (!mouse_down) {
            st.dragging = false;
            st.handle = -1;
        } else if (st.handle == 8) {
            // Body move
            ImVec2 src_mouse = screen_to_src(mouse);
            float w = rect.z - rect.x;
            float h = rect.w - rect.y;
            float nx = src_mouse.x - st.anchor.x;
            float ny = src_mouse.y - st.anchor.y;
            rect = ImVec4(nx, ny, nx + w, ny + h);
        } else if (st.handle == 9) {
            // Initial draw
            ImVec2 src_mouse = screen_to_src(mouse);
            rect = ImVec4(st.anchor.x, st.anchor.y, src_mouse.x, src_mouse.y);
            // Normalize in case the user drags right-to-left or bottom-to-top
            if (rect.z < rect.x) std::swap(rect.x, rect.z);
            if (rect.w < rect.y) std::swap(rect.y, rect.w);
        } else {
        // Handle drag. Corners (0-3) use the stored anchor; edges (4-7)
        // mutate only the dragged edge and preserve the rest of the rect.
        ImVec2 src_mouse = screen_to_src(mouse);
        float ax = st.anchor.x, ay = st.anchor.y;
        float mx = src_mouse.x, my = src_mouse.y;
        switch (st.handle) {
            case 0: rect = ImVec4(mx, my, ax, ay); break; // TL drag
            case 1: rect = ImVec4(ax, my, mx, ay); break; // TR drag
            case 2: rect = ImVec4(ax, ay, mx, my); break; // BR drag
            case 3: rect = ImVec4(mx, ay, ax, my); break; // BL drag
            case 4: rect = ImVec4(rect.x, my, rect.z, rect.w); break; // T
            case 5: rect = ImVec4(rect.x, rect.y, mx, rect.w); break; // R
            case 6: rect = ImVec4(rect.x, rect.y, rect.z, my); break; // B
            case 7: rect = ImVec4(mx, rect.y, rect.z, rect.w); break; // L
        }
        // Normalize in case the drag inverted the rect
        if (rect.z < rect.x) std::swap(rect.x, rect.z);
        if (rect.w < rect.y) std::swap(rect.y, rect.w);
        }
    }

    rect = clamp_rect(rect);
    return action;
}

void Steam_Overlay::render_pinned_screenshot()
{
    if (pinned_screenshots.empty())
        return;

    ImGui::PushFont(font_default);

    // Track overlay state transitions once — applies to all pin windows.
    static bool prev_overlay_state = false;
    bool overlay_opened = show_overlay && !prev_overlay_state;
    bool overlay_closed = !show_overlay && prev_overlay_state;
    prev_overlay_state = show_overlay;

    // Window-decoration overhead (needed for stable sizing)
    const float pad_x = 2.0f * ImGui::GetStyle().WindowPadding.x;
    const float pad_y = 2.0f * ImGui::GetStyle().WindowPadding.y;
    const float title_bar_h = show_overlay ? ImGui::GetFrameHeight() : 0;
    // Controls (separator + opacity slider) — accurately measured so no gap shows
    const float controls_h = ImGui::GetFrameHeightWithSpacing()        // slider + its trailing spacing
        + ImGui::GetStyle().ItemSpacing.y;                              // spacing above separator

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!show_overlay) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
               | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
               | ImGuiWindowFlags_NoScrollbar
               | ImGuiWindowFlags_NoBringToFrontOnFocus
               | ImGuiWindowFlags_NoFocusOnAppearing
               | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoNavInputs;
    } else {
        flags |= ImGuiWindowFlags_NoScrollbar;
    }

    // Phase 1: render each pin in its own window
    for (auto& pin : pinned_screenshots) {
        if (!pin.texture || pin.texture->GetResourceId() == 0)
            continue;

        char wnd_id[64];
        snprintf(wnd_id, sizeof(wnd_id), translationPinnedScreenshots[current_language],
                 (unsigned long long)pin.id);

        // Position (deferred until first manual move)
        bool first_frame = !pin.pos_set;

        // Always snap window to image display size when not actively resizing
        // (lets user resize while mouse is held, trims excess on release)
        bool mouse_down = ImGui::IsMouseDown(0);
        if (!mouse_down || first_frame) {
            ImVec2 img = pin.image_disp;
            bool valid = img.x >= 50.0f && img.y >= 30.0f;
            if (!valid && pin.pixels_w > 0 && pin.pixels_h > 0 && pin.size.x > 0 && pin.size.y > 0) {
                // Fallback: derive from pin.size via aspect ratio.
                // If no crop rect set, use the full image dimensions.
                float fw = (float)pin.pixels_w, fh = (float)pin.pixels_h;
                float crop_w = (pin.crop_rect.z > pin.crop_rect.x)
                             ? (pin.crop_rect.z - pin.crop_rect.x) : fw;
                float crop_h = (pin.crop_rect.w > pin.crop_rect.y)
                             ? (pin.crop_rect.w - pin.crop_rect.y) : fh;
                crop_w = std::max(1.0f, crop_w);
                crop_h = std::max(1.0f, crop_h);
                float s = std::min(pin.size.x / crop_w, pin.size.y / crop_h);
                img = ImVec2(crop_w * s, crop_h * s);
                valid = img.x >= 50.0f && img.y >= 30.0f;
            }
            if (!valid) {
                img = ImVec2(std::max(pin.size.x, 50.0f), std::max(pin.size.y, 30.0f));
            }

            // New outer window size after snap
            ImVec2 new_size = show_overlay
                ? ImVec2(img.x + pad_x, img.y + controls_h + pad_y + title_bar_h)
                : ImVec2(img.x + pad_x, img.y + pad_y);

            // Re-center window on the image so both left and right edges
            // adjust toward the image when the resize grip is released.
            // pin.size is the content area from the previous frame; the
            // previous outer size adds the same overhead we just applied.
            if (pin.size.x > 0 && pin.size.y > 0) {
                pin.pos.x += (pin.size.x - img.x) * 0.5f;
                pin.pos.y += (pin.size.y - img.y) * 0.5f;
            }

            ImGui::SetNextWindowPos(pin.pos, ImGuiCond_Always);

            // Hysteresis: only change window size when the difference is
            // meaningful (>0.5px).  The snap+rendering feedback loop
            // (SetNextWindowSize → outer → avail → image_disp → new_size)
            // can accumulate sub-pixel float error frame by frame,
            // causing continuous visible shrinking/growing.  Skipping
            // SetNextWindowSize when the change is negligible locks the
            // window to its current actual size and breaks the loop.
            if (first_frame ||
                pin.last_outer.x == 0.0f || pin.last_outer.y == 0.0f ||
                fabsf(new_size.x - pin.last_outer.x) > 0.5f ||
                fabsf(new_size.y - pin.last_outer.y) > 0.5f) {
                ImGui::SetNextWindowSize(new_size, ImGuiCond_Always);
            }
        } else {
            ImGui::SetNextWindowPos(pin.pos, first_frame
                ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        }
        pin.pos_set = true;

        ImGui::SetNextWindowSizeConstraints(ImVec2(100, 60), ImVec2(8192, 8192));
        ImGui::SetNextWindowBgAlpha(pin.opacity);

        // In crop mode, prevent the user from accidentally moving/resizing the
        // pin window (window movement during drag-to-select is confusing).
        ImGuiWindowFlags crop_flags = flags;
        if (pin.crop_mode) {
            crop_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        }

        if (ImGui::Begin(wnd_id, &pin.open, crop_flags)) {
            // Bring pin to front when requested (new pin, overlay opens)
            if (overlay_opened || pin.focus_requested) {
                ImGui::SetWindowFocus(wnd_id);
                pin.focus_requested = false;
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();
            float image_avail_y = show_overlay ? avail.y - controls_h : avail.y;

            // Normalize crop_rect: (0,0,0,0) = no crop (show full image).
            // Non-zero but inverted rects are clamped to image bounds as a safety net.
            if (pin.crop_rect.z <= pin.crop_rect.x || pin.crop_rect.w <= pin.crop_rect.y) {
                if (pin.crop_rect.x != 0 || pin.crop_rect.y != 0 ||
                    pin.crop_rect.z != 0 || pin.crop_rect.w != 0) {
                    // Non-zero degenerate rect — clamp to bounds
                    pin.crop_rect.x = std::max(0.0f, std::min((float)pin.pixels_w,  pin.crop_rect.x));
                    pin.crop_rect.y = std::max(0.0f, std::min((float)pin.pixels_h,  pin.crop_rect.y));
                    pin.crop_rect.z = std::max(pin.crop_rect.x, std::min((float)pin.pixels_w,  pin.crop_rect.z));
                    pin.crop_rect.w = std::max(pin.crop_rect.y, std::min((float)pin.pixels_h,  pin.crop_rect.w));
                }
                // Zero rect (0,0,0,0) is left as-is — means "no crop"
            }

            // In crop_mode we render the FULL image so the user can see the source
            // to crop from; the dim overlay + selection rect is drawn on top.
            // Otherwise we render only the cropped region via UV mapping.
            // (0,0,0,0) crop_rect = no crop = show full image.
            bool has_crop = pin.crop_rect.z > pin.crop_rect.x
                         && pin.crop_rect.w > pin.crop_rect.y;
            float src_w = (pin.crop_mode || !has_crop) ? (float)pin.pixels_w
                                                       : (pin.crop_rect.z - pin.crop_rect.x);
            float src_h = (pin.crop_mode || !has_crop) ? (float)pin.pixels_h
                                                       : (pin.crop_rect.w - pin.crop_rect.y);
            ImVec2 uv0 = (pin.crop_mode || !has_crop) ? ImVec2(0, 0)
                                                       : ImVec2(pin.crop_rect.x / (float)pin.pixels_w,
                                                                pin.crop_rect.y / (float)pin.pixels_h);
            ImVec2 uv1 = (pin.crop_mode || !has_crop) ? ImVec2(1, 1)
                                                       : ImVec2(pin.crop_rect.z / (float)pin.pixels_w,
                                                                pin.crop_rect.w / (float)pin.pixels_h);

            // Draw image at correct aspect ratio within available space
            ImVec2 img_screen_pos = ImVec2(0, 0);
            ImVec2 img_screen_size = ImVec2(0, 0);
            if (pin.pixels_w > 0 && pin.pixels_h > 0 && avail.x > 0 && image_avail_y > 0
                && src_w > 0 && src_h > 0) {
                float scale = std::min(avail.x / src_w, image_avail_y / src_h);
                float disp_w = src_w * scale;
                float disp_h = src_h * scale;

                // Snap the constrained axis to the exact avail dimension to
                // prevent float-arithmetic drift: src_w * (avail.x / src_w)
                // may not equal avail.x exactly in IEEE 754, causing a tiny
                // sub-pixel error every frame that compounds.
                if (avail.x * src_h <= image_avail_y * src_w)
                    disp_w = avail.x;   // width-constrained
                else
                    disp_h = image_avail_y;  // height-constrained

                float off_x = (avail.x - disp_w) * 0.5f;
                if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                dl->AddImage(pin.texture->GetResourceId(), p0,
                    ImVec2(p0.x + disp_w, p0.y + disp_h),
                    uv0, uv1,
                    IM_COL32(255, 255, 255, (int)(pin.opacity * 255.0f)));
                ImGui::Dummy(ImVec2(disp_w, disp_h));

                // Track actual image display size for closed-state shrink-wrapping
                pin.image_disp = ImVec2(disp_w, disp_h);
                img_screen_pos = p0;
                img_screen_size = ImVec2(disp_w, disp_h);
            }

            if (pin.crop_mode) {
                if (img_screen_size.x > 0 && img_screen_size.y > 0
                    && pin.pixels_w > 0 && pin.pixels_h > 0) {
                    ImTextureID tex = pin.texture ? pin.texture->GetResourceId() : 0;
                    CropAction act = render_crop_editor(pin.crop_rect, pin.crop_drag,
                        tex, pin.pixels_w, pin.pixels_h,
                        img_screen_pos, img_screen_size);
                    if (act == CropAction::Confirm) {
                        // Clamp the final rect to source bounds (clamp_rect is
                        // already called inside render_crop_editor, so this is
                        // a no-op safety net)
                        pin.crop_mode = false;
                        pin.focus_requested = false;
                        // Reset image_disp so the next frame recalculates the
                        // window size from the (now smaller) crop_rect instead
                        // of reusing the last crop-mode full-image size.
                        pin.image_disp = ImVec2(0, 0);
                    } else if (act == CropAction::Cancel) {
                        pin.crop_rect = pin.crop_rect_prev;
                        pin.crop_mode = false;
                    }
                }
            } else if (show_overlay) {
                ImGui::Separator();
                ImGui::SliderFloat(translationOpacity[current_language], &pin.opacity, 0.1f, 1.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button(translationCrop[current_language])) {
                    pin.crop_rect_prev = pin.crop_rect;
                    pin.crop_mode = true;
                }
            }

            if (show_overlay) {
                pin.pos = ImGui::GetWindowPos();
                // Use the actual content region avail (from GetContentRegionAvail)
                // instead of deriving from outer size minus estimated overhead,
                // so there is zero mismatch between what the rendering sees
                // and what the snap uses for sizing and re-centering next frame.
                pin.size = ImVec2(avail.x, image_avail_y);
                if (pin.size.x < 50.0f) pin.size.x = 50.0f;
                if (pin.size.y < 30.0f) pin.size.y = 30.0f;
            } else {
                // Track position and size even with overlay closed so the first
                // overlay-on frame doesn't work with stale values.
                pin.pos = ImGui::GetWindowPos();
                pin.size = ImVec2(avail.x, avail.y);
            }
            // Record the actual window outer size for the next frame's
            // hysteresis comparison.  Must come after the window has been
            // fully sized and positioned by ImGui::Begin+SetNextWindowSize.
            pin.last_outer = ImGui::GetWindowSize();
        }
        ImGui::End();
    }

    // Phase 2: remove any pins whose X button was clicked
    for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
        if (!it->open) {
            if (it->texture) {
                if (it->texture->GetResourceId() != 0)
                    it->texture->Unload();
                it->texture->Delete();
            }
            it = pinned_screenshots.erase(it);
        } else {
            ++it;
        }
    }

    ImGui::PopFont();
}


#endif
