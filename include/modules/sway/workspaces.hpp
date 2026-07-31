#pragma once

#include <fmt/format.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
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

class Workspaces : public AModule, public sigc::trackable {
 public:
  Workspaces(const std::string&, const waybar::Bar&, const Json::Value&);
  ~Workspaces() override = default;
  auto update() -> void override;

 private:
  static constexpr std::string_view workspace_switch_cmd_ = "workspace {} \"{}\"";
  static constexpr std::string_view workspace_switch_number_cmd_ = "workspace {} number {}";
  static constexpr std::string_view persistent_workspace_switch_cmd_ =
      R"(workspace {} "{}"; move workspace to output "{}"; workspace {} "{}")";

  // The button of a single workspace. Its text lives in the auto-created child label.
  struct WorkspaceButton {
    explicit WorkspaceButton(const std::string& name) : button(name) {}
    Gtk::Label& labelWidget();

    Gtk::Button button;
  };

  static int convertWorkspaceNameToNum(const std::string& name);
  static int windowRewritePriorityFunction(std::string const& window_rule);

  auto populateIgnoreWorkspacesConfig(const Json::Value& config) -> void;
  auto populateWorkspaceTaskbarConfig(const Json::Value& config) -> void;
  static void collectWindows(const Json::Value& node, const WorkspaceTaskbarConfig& config,
                             std::vector<WorkspaceWindow>& windows);
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
  std::unordered_map<std::string, uint16_t> custom_sort_priorities_;
  std::mutex mutex_;
  Ipc ipc_;
};

}  // namespace waybar::modules::sway
