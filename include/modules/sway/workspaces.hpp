#pragma once

#include <fmt/format.h>
#include <giomm/desktopappinfo.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "client.hpp"
#include "modules/sway/ipc/client.hpp"
#include "util/icon_loader.hpp"
#include "util/json.hpp"
#include "util/regex_collection.hpp"

namespace waybar::modules::sway {

// Plain data describing one view (window) of a workspace, parsed out of the sway IPC tree.
// Contains no GTK state.
struct WorkspaceWindow {
  int64_t id = -1;
  std::string title;
  // Native Wayland views report an app_id; for XWayland views sway reports the X11
  // instance and class hints instead. app_id holds the app_id (or the instance),
  // app_class the class, which is the better icon lookup key.
  std::string app_id;
  std::string app_class;
  bool focused = false;
  bool fullscreen = false;
  bool urgent = false;
};

// Parsed form of the "workspace-taskbar" config object. Written once during construction
// and read-only afterwards. Left default-constructed when the mode is disabled.
struct WorkspaceTaskbarConfig {
  bool enable = false;
  // "format" split once around {icon}, so the image sits between two labels.
  bool with_icon = false;
  std::string format_before;
  std::string format_after;
  // Empty when tooltips are disabled.
  std::string format_tooltip;
  int icon_size = 16;
  bool markup = false;
  // Matched against a window's app_id, class and title; a hit hides the window.
  std::vector<std::regex> ignore_list;
  std::map<std::string, std::string> app_ids_replace;
  Json::Value rewrite;
  std::string on_click = "activate";
  std::string on_click_middle;
  std::string on_click_right;
};

class Workspaces;

// One window of a workspace, rendered as a button nested inside that workspace's button.
class WorkspaceWindowButton {
 public:
  WorkspaceWindowButton(Workspaces& owner, Gtk::Orientation orientation,
                        const WorkspaceWindow& window);

  int64_t id() const { return id_; }
  Gtk::Button& widget() { return button_; }

  // Refresh the cached snapshot, resolving the desktop entry if the window's identity
  // changed. Only ever called from the main thread.
  void setData(const WorkspaceWindow& window);
  // Apply the cached snapshot to the icon, labels, tooltip and style classes. Must run
  // with the button already packed: loading the icon reads the widget's scale factor,
  // which is only correct once it has a parent.
  void render();

 private:
  // The pieces of a window's snapshot that user format strings can interpolate.
  struct FormatArgs {
    std::string title;
    std::string name;
    std::string app_id;
    std::string state;
    std::string short_state;
  };

  std::string stateString(bool shortened) const;
  // Expand one user-supplied format string. A malformed format makes fmt throw; this
  // yields an empty string rather than letting the exception escape update().
  std::string formatText(const std::string& format, const FormatArgs& args) const;
  bool handleClicked(GdkEventButton* event);

  Workspaces& owner_;
  int64_t id_;

  Gtk::Button button_;
  Gtk::Box content_;
  Gtk::Image icon_;
  Gtk::Label text_before_;
  Gtk::Label text_after_;

  Glib::RefPtr<Gio::DesktopAppInfo> app_info_;
  // False until setData() has resolved app_info_/name_ once, so a window whose app_id is
  // empty still gets its initial resolution.
  bool app_info_resolved_ = false;
  // Set when app_info_ changed, cleared once render() has loaded the matching icon.
  bool icon_dirty_ = false;
  std::string name_;
  std::string title_;
  std::string app_id_;
  std::string app_class_;
  bool focused_ = false;
  bool fullscreen_ = false;
  bool urgent_ = false;
};

class Workspaces : public AModule, public sigc::trackable {
 public:
  Workspaces(const std::string&, const waybar::Bar&, const Json::Value&);
  ~Workspaces() override = default;
  auto update() -> void override;

  // Used by the nested window buttons.
  Ipc& ipc() { return ipc_; }
  const IconLoader& iconLoader() const { return icon_loader_; }
  const WorkspaceTaskbarConfig& taskbarConfig() const { return taskbar_config_; }
  // Desktop entry for a window, memoised: the lookup scans desktop files, and a window
  // moving between workspaces rebuilds its button.
  Glib::RefPtr<Gio::DesktopAppInfo> resolveAppInfo(const std::string& app_id,
                                                   const std::string& app_class);
  // Warns at most once per module, however many windows share the malformed format. Called
  // from update(), which runs from a Glib::Dispatcher callback where an escaping exception
  // would terminate waybar.
  void warnTaskbarFormat(const std::string& format, const std::string& error);

 private:
  static constexpr std::string_view workspace_switch_cmd_ = "workspace {} \"{}\"";
  static constexpr std::string_view workspace_switch_number_cmd_ = "workspace {} number {}";
  static constexpr std::string_view persistent_workspace_switch_cmd_ =
      R"(workspace {} "{}"; move workspace to output "{}"; workspace {} "{}")";

  // The button of a single workspace. Without the taskbar its text lives in the button's
  // auto-created child label; with the taskbar the button gets an explicit box holding the
  // label and the window buttons instead.
  //
  // Never call Gtk::Button::set_label() on `button` once that explicit child exists --
  // gtk_button_construct_child() would destroy it. Write text through labelWidget().
  struct WorkspaceButton {
    WorkspaceButton(const std::string& name, bool taskbar_enabled, Gtk::Orientation orientation);
    Gtk::Label& labelWidget();

    Gtk::Button button;
    // Only parented when taskbar_mode is set; otherwise they are constructed and left
    // unused, which is cheaper than making them conditionally allocated.
    Gtk::Box content;
    Gtk::Label label;
    Gtk::Box taskbar;
    bool taskbar_mode = false;
    // Declared last so it is destroyed first: each window button unparents itself from
    // `taskbar`, which must still be alive at that point.
    std::vector<std::unique_ptr<WorkspaceWindowButton>> windows;
  };

  static int convertWorkspaceNameToNum(const std::string& name);
  static int windowRewritePriorityFunction(std::string const& window_rule);

  auto populateIgnoreWorkspacesConfig(const Json::Value& config) -> void;
  auto populateWorkspaceTaskbarConfig(const Json::Value& config) -> void;
  static void collectWindows(const Json::Value& node, const WorkspaceTaskbarConfig& config,
                             std::vector<WorkspaceWindow>& windows);
  void updateWorkspaceTaskbar(WorkspaceButton& ws, const std::string& name);
  bool isWorkspaceIgnored(std::string const& name);
  void onCmd(const struct Ipc::ipc_response&);
  void onEvent(const struct Ipc::ipc_response&);
  bool filterButtons();
  static bool hasFlag(const Json::Value&, const std::string&);
  static bool isWorkspaceEmpty(const Json::Value&);
  static bool isWorkspaceVisible(const Json::Value&);
  static bool hasState(const Json::Value&, const std::string&);
  void updateWindows(const Json::Value&, std::string&);
  WorkspaceButton& addButton(const Json::Value&);
  void onButtonReady(const Json::Value&, WorkspaceButton&);
  std::string getIcon(const std::string&, const Json::Value&);
  std::string getCycleWorkspace(std::vector<Json::Value>::iterator, bool prev) const;
  uint16_t getWorkspaceIndex(const std::string& name) const;
  static std::string trimWorkspaceName(const std::string&);
  std::optional<uint16_t> getCustomSortIndex(const std::string& name) const;
  bool handleScroll(GdkEventScroll* /*unused*/) override;

  const Bar& bar_;
  std::vector<Json::Value> workspaces_;
  std::vector<std::string> high_priority_named_;
  std::vector<std::string> workspaces_order_;
  Gtk::Box box_;
  std::string m_formatWindowSeparator;
  std::vector<std::regex> m_ignoreWorkspaces;
  util::RegexCollection m_windowRewriteRules;
  util::JsonParser parser_;
  std::unordered_map<std::string, WorkspaceButton> buttons_;
  WorkspaceTaskbarConfig taskbar_config_;
  IconLoader icon_loader_;
  // Windows of each workspace, keyed by workspace name. Rebuilt by onCmd() and consumed by
  // update(); guarded by mutex_ alongside workspaces_, which it is derived from.
  std::unordered_map<std::string, std::vector<WorkspaceWindow>> workspace_windows_;
  std::map<std::pair<std::string, std::string>, Glib::RefPtr<Gio::DesktopAppInfo>> app_info_cache_;
  bool taskbar_format_warned_ = false;
  std::unordered_map<std::string, uint16_t> custom_sort_priorities_;
  std::mutex mutex_;
  Ipc ipc_;
};

}  // namespace waybar::modules::sway
