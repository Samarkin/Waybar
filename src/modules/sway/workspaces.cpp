#include "modules/sway/workspaces.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "util/rewrite_string.hpp"
#include "util/sanitize_str.hpp"
#include "util/string.hpp"

namespace waybar::modules::sway {

namespace {

// Returns {app_id, app_class} for a view. Native Wayland views carry an `app_id`; XWayland
// views instead carry the X11 `instance` and `class` hints. The instance is used as the
// displayed app_id (as sway/window does), but the class is carried along because it is the
// identifier icon lookup actually needs: X11 Firefox reports instance "Navigator" and
// class "firefox".
std::pair<std::string, std::string> resolve_app_id(
    const Json::Value& node, const std::map<std::string, std::string>& replace_map) {
  std::string app_id;
  std::string app_class;
  if (node["window_properties"]["class"].isString()) {
    app_class = node["window_properties"]["class"].asString();
  }
  if (node["app_id"].isString()) {
    app_id = node["app_id"].asString();
  } else if (node["window_properties"]["instance"].isString()) {
    app_id = node["window_properties"]["instance"].asString();
  } else {
    app_id = app_class;
  }

  const auto replace = [&replace_map](std::string& value) {
    const auto it = replace_map.find(value);
    if (it != replace_map.end()) {
      value = it->second;
    }
  };
  replace(app_id);
  replace(app_class);
  return {app_id, app_class};
}

// A view with no children of its own. Unlike the predicate updateWindows() uses, this
// matches nameless views but not named containers.
bool is_leaf_view(const Json::Value& node) {
  const auto type = node["type"].asString();
  return (type == "con" || type == "floating_con") && node["nodes"].empty() &&
         node["floating_nodes"].empty();
}

bool is_window_ignored(const std::vector<std::regex>& ignore_list, const WorkspaceWindow& window) {
  return std::any_of(ignore_list.begin(), ignore_list.end(), [&window](const std::regex& rule) {
    return std::regex_match(window.app_id, rule) || std::regex_match(window.app_class, rule) ||
           std::regex_match(window.title, rule);
  });
}

}  // namespace

WorkspaceWindowButton::WorkspaceWindowButton(Workspaces& owner, Gtk::Orientation orientation,
                                             const WorkspaceWindow& window)
    : owner_(owner), id_(window.id), content_(orientation, 0) {
  button_.set_relief(Gtk::RELIEF_NONE);
  button_.get_style_context()->add_class("taskbar-window");

  content_.add(text_before_);
  content_.add(icon_);
  content_.add(text_after_);
  content_.show();
  button_.add(content_);

  button_.signal_button_release_event().connect(
      sigc::mem_fun(*this, &WorkspaceWindowButton::handleClicked), false);

  setData(window);
}

std::string WorkspaceWindowButton::stateString(bool shortened) const {
  std::stringstream ss;
  if (shortened) {
    ss << (focused_ ? "A" : "") << (fullscreen_ ? "F" : "") << (urgent_ ? "U" : "");
  } else {
    ss << (focused_ ? "active " : "") << (fullscreen_ ? "fullscreen " : "")
       << (urgent_ ? "urgent " : "");
  }

  std::string res = ss.str();
  if (shortened || res.empty()) {
    return res;
  }
  return res.substr(0, res.size() - 1);
}

void WorkspaceWindowButton::setData(const WorkspaceWindow& window) {
  // The desktop entry and the icon are keyed on the identifiers alone, so they are only
  // re-resolved when those actually change. Titles change on every page navigation or
  // shell command; re-scanning desktop files for those would cost a lookup per keystroke.
  const bool identity_changed =
      !app_info_resolved_ || app_id_ != window.app_id || app_class_ != window.app_class;

  title_ = window.title;
  app_id_ = window.app_id;
  app_class_ = window.app_class;
  focused_ = window.focused;
  fullscreen_ = window.fullscreen;
  urgent_ = window.urgent;

  if (!identity_changed) {
    return;
  }
  app_info_resolved_ = true;

  app_info_ = owner_.resolveAppInfo(app_id_, app_class_);
  name_ = app_info_ ? app_info_->get_display_name() : app_id_;
  // Loading the icon is left to render(), which runs once the button has a parent and so
  // can read the right scale factor for the output the bar is on.
  icon_dirty_ = owner_.taskbarConfig().with_icon;
}

std::string WorkspaceWindowButton::formatText(const std::string& format,
                                              const FormatArgs& args) const {
  try {
    return fmt::format(fmt::runtime(format), fmt::arg("title", args.title),
                       fmt::arg("name", args.name), fmt::arg("app_id", args.app_id),
                       fmt::arg("state", args.state), fmt::arg("short_state", args.short_state));
  } catch (const std::exception& e) {
    owner_.warnTaskbarFormat(format, e.what());
    return {};
  }
}

void WorkspaceWindowButton::render() {
  const auto& config = owner_.taskbarConfig();

  if (icon_dirty_) {
    icon_dirty_ = false;
    if (owner_.iconLoader().image_load_icon(icon_, app_info_, config.icon_size)) {
      icon_.show();
    } else {
      icon_.hide();
      spdlog::debug("Couldn't find icon for {}", app_id_);
    }
  }

  FormatArgs args{title_, name_, app_id_, stateString(false), stateString(true)};
  if (config.markup) {
    args.title = util::sanitize_string(args.title);
    args.name = util::sanitize_string(args.name);
    args.app_id = util::sanitize_string(args.app_id);
  }

  const auto write = [&](Gtk::Label& label, const std::string& format) {
    if (format.empty()) {
      label.hide();
      return;
    }
    auto text = util::rewriteString(formatText(format, args), config.rewrite);
    if (config.markup) {
      label.set_markup(text);
    } else {
      label.set_text(text);
    }
    // An empty expansion (a bad format, or a rewrite rule mapping to "") would otherwise
    // leave a zero-width label parented for no reason.
    if (text.empty()) {
      label.hide();
    } else {
      label.show();
    }
  };
  write(text_before_, config.format_before);
  write(text_after_, config.format_after);

  if (!config.format_tooltip.empty()) {
    auto text = util::rewriteString(formatText(config.format_tooltip, args), config.rewrite);
    if (config.markup) {
      button_.set_tooltip_markup(text);
    } else {
      button_.set_tooltip_text(text);
    }
  }

  if (focused_) {
    button_.get_style_context()->add_class("active");
  } else {
    button_.get_style_context()->remove_class("active");
  }
}

bool WorkspaceWindowButton::handleClicked(GdkEventButton* event) {
  const auto& config = owner_.taskbarConfig();
  std::string action;
  if (event->button == 1) {
    action = config.on_click;
  } else if (event->button == 2) {
    action = config.on_click_middle;
  } else if (event->button == 3) {
    action = config.on_click_right;
  }

  if (action.empty()) {
    return true;
  }

  try {
    if (action == "activate") {
      owner_.ipc().sendCmd(IPC_COMMAND, fmt::format("[con_id={}] focus", id_));
    } else if (action == "close") {
      owner_.ipc().sendCmd(IPC_COMMAND, fmt::format("[con_id={}] kill", id_));
    } else if (action == "fullscreen") {
      owner_.ipc().sendCmd(IPC_COMMAND, fmt::format("[con_id={}] fullscreen toggle", id_));
    } else if (action == "minimize" || action == "minimize-raise" || action == "maximize") {
      spdlog::warn("{} is not supported on sway", action);
    } else {
      spdlog::warn("Unknown action {}", action);
    }
  } catch (const std::exception& e) {
    spdlog::error("Workspaces: {}", e.what());
  }

  // A nested button consumes the whole click sequence, so the workspace button's own
  // handler never runs for clicks landing here -- including when no action is configured.
  return true;
}

Workspaces::WorkspaceButton::WorkspaceButton(const std::string& name, bool taskbar_enabled,
                                             Gtk::Orientation orientation)
    : content(orientation, 0), label(name), taskbar(orientation, 0), taskbar_mode(taskbar_enabled) {
  if (!taskbar_mode) {
    // Let the button create its own child label, so that "#workspaces button > label" and
    // friends keep matching. content/label/taskbar stay unparented and unused.
    button.set_label(name);
    return;
  }
  label.get_style_context()->add_class("workspace-label");
  content.pack_start(label, false, false, 0);
  content.pack_start(taskbar, false, false, 0);
  button.add(content);
  content.show();
  label.show();
  taskbar.show();
}

Gtk::Label& Workspaces::WorkspaceButton::labelWidget() {
  return taskbar_mode ? label : *static_cast<Gtk::Label*>(button.get_children()[0]);
}

// Helper function to assign a number to a workspace, just like sway. In fact
// this is taken quite verbatim from `sway/ipc-json.c`.
int Workspaces::convertWorkspaceNameToNum(const std::string& name) {
  if (isdigit(name[0]) != 0) {
    errno = 0;
    char* endptr = nullptr;
    long long parsed_num = strtoll(name.c_str(), &endptr, 10);
    if (errno != 0 || parsed_num > INT32_MAX || parsed_num < 0 || endptr == name.c_str()) {
      return -1;
    }
    return (int)parsed_num;
  }
  return -1;
}

int Workspaces::windowRewritePriorityFunction(std::string const& window_rule) {
  // Rules that match against title are prioritized
  // Rules that don't specify if they're matching against either title or class are deprioritized
  bool const hasTitle = window_rule.find("title") != std::string::npos;
  bool const hasClass = window_rule.find("class") != std::string::npos;

  if (hasTitle && hasClass) {
    return 3;
  }
  if (hasTitle) {
    return 2;
  }
  if (hasClass) {
    return 1;
  }
  return 0;
}

Workspaces::Workspaces(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule(config, "workspaces", id, false, !config["disable-scroll"].asBool()),
      bar_(bar),
      box_(bar.orientation, 0) {
  if (config["format-icons"]["high-priority-named"].isArray()) {
    for (const auto& it : config["format-icons"]["high-priority-named"]) {
      high_priority_named_.push_back(it.asString());
    }
  }
  if (config_["custom-sort"].isArray()) {
    uint16_t priority = 0;
    for (const auto& entry : config_["custom-sort"]) {
      if (!entry.isString()) {
        continue;
      }
      auto const name = entry.asString();
      custom_sort_priorities_.insert_or_assign(name, priority);
      auto const trimmed = trimWorkspaceName(name);
      if (trimmed != name) {
        custom_sort_priorities_.insert_or_assign(trimmed, priority);
      }
      ++priority;
    }
  }
  box_.set_name("workspaces");
  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }
  box_.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(box_);
  if (config_["format-window-separator"].isString()) {
    m_formatWindowSeparator = config_["format-window-separator"].asString();
  } else {
    m_formatWindowSeparator = " ";
  }
  const Json::Value& windowRewrite = config["window-rewrite"];
  if (windowRewrite.isObject()) {
    const Json::Value& windowRewriteDefaultConfig = config["window-rewrite-default"];
    std::string windowRewriteDefault =
        windowRewriteDefaultConfig.isString() ? windowRewriteDefaultConfig.asString() : "?";
    m_windowRewriteRules = waybar::util::RegexCollection(
        windowRewrite, std::move(windowRewriteDefault), windowRewritePriorityFunction);
  }
  populateIgnoreWorkspacesConfig(config);
  populateWorkspaceTaskbarConfig(config);
  ipc_.subscribe(R"(["workspace"])");
  ipc_.subscribe(R"(["window"])");
  ipc_.signal_event.connect(sigc::mem_fun(*this, &Workspaces::onEvent));
  ipc_.signal_cmd.connect(sigc::mem_fun(*this, &Workspaces::onCmd));
  ipc_.sendCmd(IPC_GET_TREE);
  if (config["enable-bar-scroll"].asBool()) {
    auto& window = const_cast<Bar&>(bar_).window;
    window.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
    window.signal_scroll_event().connect(sigc::mem_fun(*this, &Workspaces::handleScroll));
  }
  // Launch worker
  ipc_.setWorker([this] {
    try {
      ipc_.handleEvent();
    } catch (const std::exception& e) {
      spdlog::error("Workspaces: {}", e.what());
    }
  });
}

void Workspaces::onEvent(const struct Ipc::ipc_response& res) {
  try {
    ipc_.sendCmd(IPC_GET_TREE);
  } catch (const std::exception& e) {
    spdlog::error("Workspaces: {}", e.what());
  }
}

auto Workspaces::populateIgnoreWorkspacesConfig(const Json::Value& config) -> void {
  auto ignoreWorkspaces = config["ignore-workspaces"];
  if (ignoreWorkspaces.isArray()) {
    for (const auto& workspaceRegex : ignoreWorkspaces) {
      if (workspaceRegex.isString()) {
        std::string ruleString = workspaceRegex.asString();
        try {
          const std::regex rule{ruleString, std::regex_constants::icase};
          m_ignoreWorkspaces.emplace_back(rule);
        } catch (const std::regex_error& e) {
          spdlog::error("Invalid rule {}: {}", ruleString, e.what());
        }
      } else {
        spdlog::error("Not a string: '{}'", workspaceRegex);
      }
    }
  }
}

auto Workspaces::populateWorkspaceTaskbarConfig(const Json::Value& config) -> void {
  const Json::Value& taskbar = config["workspace-taskbar"];
  if (!taskbar.isObject()) {
    return;
  }

  taskbar_config_.enable = taskbar["enable"].isBool() && taskbar["enable"].asBool();
  if (!taskbar_config_.enable) {
    return;
  }

  if (taskbar["format"].isString()) {
    auto parts = split(taskbar["format"].asString(), "{icon}", 1);
    taskbar_config_.format_before = parts[0];
    if (parts.size() > 1) {
      taskbar_config_.with_icon = true;
      taskbar_config_.format_after = parts[1];
    }
  } else {
    // The default is to only show the icon.
    taskbar_config_.with_icon = true;
  }

  if (!taskbar["tooltip"].isBool() || taskbar["tooltip"].asBool()) {
    taskbar_config_.format_tooltip =
        taskbar["tooltip-format"].isString() ? taskbar["tooltip-format"].asString() : "{title}";
  }

  if (taskbar["icon-size"].isInt()) {
    taskbar_config_.icon_size = taskbar["icon-size"].asInt();
  }
  taskbar_config_.markup = taskbar["markup"].isBool() && taskbar["markup"].asBool();
  taskbar_config_.rewrite = taskbar["rewrite"];

  if (taskbar["icon-theme"].isArray()) {
    for (const auto& theme : taskbar["icon-theme"]) {
      if (theme.isString()) {
        icon_loader_.add_custom_icon_theme(theme.asString());
      } else {
        spdlog::error("Not a string: '{}'", theme);
      }
    }
  } else if (taskbar["icon-theme"].isString()) {
    icon_loader_.add_custom_icon_theme(taskbar["icon-theme"].asString());
  }

  if (taskbar["ignore-list"].isArray()) {
    for (const auto& rule : taskbar["ignore-list"]) {
      if (!rule.isString()) {
        spdlog::error("Not a string: '{}'", rule);
        continue;
      }
      try {
        taskbar_config_.ignore_list.emplace_back(rule.asString(), std::regex_constants::icase);
      } catch (const std::regex_error& e) {
        spdlog::error("Invalid rule {}: {}", rule.asString(), e.what());
      }
    }
  }

  if (taskbar["app_ids-mapping"].isObject()) {
    const Json::Value& mapping = taskbar["app_ids-mapping"];
    for (const auto& app_id : mapping.getMemberNames()) {
      if (mapping[app_id].isString()) {
        taskbar_config_.app_ids_replace.emplace(app_id, mapping[app_id].asString());
      } else {
        spdlog::error("Not a string: '{}'", mapping[app_id]);
      }
    }
  }

  // These keys are nested, so AModule (which only scans top-level keys) never sees them and
  // will not also run them as shell commands. A top-level "on-click" on the module keeps
  // behaving as a command, unchanged.
  const auto read_action = [&taskbar](const char* key, std::string& out) {
    if (taskbar[key].isString()) {
      out = taskbar[key].asString();
    }
  };
  read_action("on-click", taskbar_config_.on_click);
  read_action("on-click-middle", taskbar_config_.on_click_middle);
  read_action("on-click-right", taskbar_config_.on_click_right);
}

void Workspaces::collectWindows(const Json::Value& node, const WorkspaceTaskbarConfig& config,
                                std::vector<WorkspaceWindow>& windows) {
  if (is_leaf_view(node)) {
    WorkspaceWindow window;
    window.id = node["id"].asInt64();
    window.title = node["name"].isString() ? node["name"].asString() : "";
    std::tie(window.app_id, window.app_class) = resolve_app_id(node, config.app_ids_replace);
    window.focused = node["focused"].asBool();
    window.fullscreen = node["fullscreen_mode"].asInt() != 0;
    window.urgent = node["urgent"].asBool();
    if (!is_window_ignored(config.ignore_list, window)) {
      windows.push_back(std::move(window));
    }
    return;
  }

  for (const Json::Value& child : node["nodes"]) {
    collectWindows(child, config, windows);
  }
  for (const Json::Value& child : node["floating_nodes"]) {
    collectWindows(child, config, windows);
  }
}

Glib::RefPtr<Gio::DesktopAppInfo> Workspaces::resolveAppInfo(const std::string& app_id,
                                                             const std::string& app_class) {
  const auto key = std::make_pair(app_id, app_class);
  if (const auto it = app_info_cache_.find(key); it != app_info_cache_.end()) {
    return it->second;
  }

  auto app_info = IconLoader::get_app_info_from_app_id_list(app_id);
  if (!app_info && !app_class.empty() && app_class != app_id) {
    // XWayland views report the instance as their app_id, which rarely names a desktop
    // entry; the class usually does.
    app_info = IconLoader::get_app_info_from_app_id_list(app_class);
  }
  app_info_cache_.emplace(key, app_info);
  return app_info;
}

void Workspaces::warnTaskbarFormat(const std::string& format, const std::string& error) {
  if (taskbar_format_warned_) {
    return;
  }
  taskbar_format_warned_ = true;
  spdlog::warn("Workspaces: invalid workspace-taskbar format '{}': {}", format, error);
}

void Workspaces::updateWorkspaceTaskbar(WorkspaceButton& ws, const std::string& name) {
  static const std::vector<WorkspaceWindow> kNoWindows;
  const auto it = workspace_windows_.find(name);
  const auto& windows = it == workspace_windows_.end() ? kNoWindows : it->second;

  // Drop the buttons of windows that are gone; ~Gtk::Button unparents them from `taskbar`.
  std::erase_if(ws.windows, [&windows](const std::unique_ptr<WorkspaceWindowButton>& button) {
    return std::none_of(windows.begin(), windows.end(), [&button](const WorkspaceWindow& window) {
      return window.id == button->id();
    });
  });

  int position = 0;
  for (const auto& window : windows) {
    auto existing = std::find_if(ws.windows.begin(), ws.windows.end(),
                                 [&window](const std::unique_ptr<WorkspaceWindowButton>& button) {
                                   return button->id() == window.id;
                                 });

    WorkspaceWindowButton* button = nullptr;
    if (existing == ws.windows.end()) {
      ws.windows.push_back(
          std::make_unique<WorkspaceWindowButton>(*this, bar_.orientation, window));
      button = ws.windows.back().get();
      ws.taskbar.pack_start(button->widget(), false, false, 0);
      button->widget().show();
    } else {
      button = existing->get();
      button->setData(window);
    }

    ws.taskbar.reorder_child(button->widget(), position++);
    button->render();
  }
}

bool Workspaces::isWorkspaceIgnored(std::string const& name) {
  for (auto& rule : m_ignoreWorkspaces) {
    if (std::regex_match(name, rule)) {
      return true;
      break;
    }
  }

  return false;
}

void Workspaces::onCmd(const struct Ipc::ipc_response& res) {
  if (res.type == IPC_GET_TREE) {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto payload = parser_.parse(res.payload);
        workspaces_.clear();
        std::vector<Json::Value> outputs;
        bool alloutputs = config_["all-outputs"].asBool();
        std::copy_if(payload["nodes"].begin(), payload["nodes"].end(), std::back_inserter(outputs),
                     [&](const auto& output) {
                       if (alloutputs && output["name"].asString() != "__i3") {
                         return true;
                       }
                       if (output["name"].asString() == bar_.output->name) {
                         return true;
                       }
                       return false;
                     });

        for (auto& output : outputs) {
          std::copy_if(
              output["nodes"].begin(), output["nodes"].end(), std::back_inserter(workspaces_),
              [&](const auto& node) { return !(isWorkspaceIgnored(node["name"].asString())); });
          std::copy(output["floating_nodes"].begin(), output["floating_nodes"].end(),
                    std::back_inserter(workspaces_));
        }

        // adding persistent workspaces (as per the config file)
        if (config_["persistent-workspaces"].isObject()) {
          const Json::Value& p_workspaces = config_["persistent-workspaces"];
          const std::vector<std::string> p_workspaces_names = p_workspaces.getMemberNames();

          for (const std::string& p_w_name : p_workspaces_names) {
            const Json::Value& p_w = p_workspaces[p_w_name];
            auto it = std::find_if(workspaces_.begin(), workspaces_.end(),
                                   [&p_w_name](const Json::Value& node) {
                                     return node["name"].asString() == p_w_name;
                                   });

            if (it != workspaces_.end()) {
              continue;  // already displayed by some bar
            }

            if (p_w.isArray() && !p_w.empty()) {
              // Adding to target outputs
              for (const Json::Value& output : p_w) {
                auto output_name = output.asString();
                if (output_name == bar_.output->name || output_name == bar_.output->identifier) {
                  Json::Value v;
                  v["name"] = p_w_name;
                  v["target_output"] = bar_.output->name;
                  v["num"] = convertWorkspaceNameToNum(p_w_name);
                  workspaces_.emplace_back(std::move(v));
                  break;
                }
              }
            } else {
              // Adding to all outputs
              Json::Value v;
              v["name"] = p_w_name;
              v["target_output"] = "";
              v["num"] = convertWorkspaceNameToNum(p_w_name);
              workspaces_.emplace_back(std::move(v));
            }
          }
        }

        // sway has a defined ordering of workspaces that should be preserved in
        // the representation displayed by waybar to ensure that commands such
        // as "workspace prev" or "workspace next" make sense when looking at
        // the workspace representation in the bar.
        // Due to waybar's own feature of persistent workspaces unknown to sway,
        // custom sorting logic is necessary to make these workspaces appear
        // naturally in the list of workspaces without messing up sway's
        // sorting. For this purpose, a custom numbering property is created
        // that preserves the order provided by sway while inserting numbered
        // persistent workspaces at their natural positions.
        //
        // All of this code assumes that sway provides numbered workspaces first
        // and other workspaces are sorted by their creation time.
        //
        // In a first pass, the maximum "num" value is computed to enqueue
        // unnumbered workspaces behind numbered ones when computing the sort
        // attribute.
        //
        // Note: if the 'alphabetical_sort' option is true, the user is in
        // agreement that the "workspace prev/next" commands may not follow
        // the order displayed in Waybar.
        int max_num = -1;
        for (auto& workspace : workspaces_) {
          max_num = std::max(workspace["num"].asInt(), max_num);
        }
        for (auto& workspace : workspaces_) {
          auto workspace_num = workspace["num"].asInt();
          if (workspace_num > -1) {
            workspace["sort"] = workspace_num;
          } else {
            workspace["sort"] = ++max_num;
          }
        }
        std::sort(workspaces_.begin(), workspaces_.end(),
                  [this](const Json::Value& lhs, const Json::Value& rhs) {
                    auto lname = lhs["name"].asString();
                    auto rname = rhs["name"].asString();
                    int l = lhs["sort"].asInt();
                    int r = rhs["sort"].asInt();

                    if (!custom_sort_priorities_.empty()) {
                      auto const lcustom = getCustomSortIndex(lname);
                      auto const rcustom = getCustomSortIndex(rname);
                      if (lcustom && rcustom) {
                        if (*lcustom != *rcustom) {
                          return *lcustom < *rcustom;
                        }
                      } else if (lcustom) {
                        return true;
                      } else if (rcustom) {
                        return false;
                      }
                    }

                    if (l == r || config_["alphabetical_sort"].asBool()) {
                      // In case both integers are the same, lexicographical
                      // sort. The code above already ensure that this will only
                      // happened in case of explicitly numbered workspaces.
                      //
                      // Additionally, if the config specifies to sort workspaces
                      // alphabetically do this here.
                      return lname < rname;
                    }

                    return l < r;
                  });

        workspace_windows_.clear();
        if (taskbar_config_.enable) {
          for (const auto& workspace : workspaces_) {
            collectWindows(workspace, taskbar_config_,
                           workspace_windows_[workspace["name"].asString()]);
          }
        }
      }
      dp.emit();
    } catch (const std::exception& e) {
      spdlog::error("Workspaces: {}", e.what());
    }
  }
}

bool Workspaces::filterButtons() {
  bool needReorder = false;
  for (auto it = buttons_.begin(); it != buttons_.end();) {
    auto ws = std::find_if(workspaces_.begin(), workspaces_.end(),
                           [it](const auto& node) { return node["name"].asString() == it->first; });
    if (ws == workspaces_.end() ||
        ((*ws).isMember("target_output") ? (*ws)["target_output"].asString() != bar_.output->name &&
                                               (*ws)["target_output"].asString() != ""
                                         : !config_["all-outputs"].asBool() &&
                                               (*ws)["output"].asString() != bar_.output->name)) {
      it = buttons_.erase(it);
      needReorder = true;
    } else {
      ++it;
    }
  }
  return needReorder;
}

bool Workspaces::hasFlag(const Json::Value& node, const std::string& flag) {
  if (node[flag].asBool()) {
    return true;
  }

  if (std::any_of(node["nodes"].begin(), node["nodes"].end(),
                  [&](auto const& e) { return hasFlag(e, flag); })) {
    return true;
  }
  if (std::any_of(node["floating_nodes"].begin(), node["floating_nodes"].end(),
                  [&](auto const& e) { return hasFlag(e, flag); })) {
    return true;
  }
  return false;
}

bool Workspaces::isWorkspaceEmpty(const Json::Value& node) {
  return node["nodes"].empty() && node["floating_nodes"].empty();
}

// Sway sets "visible" on views, not on workspaces, and hasFlag recurses: a workspace
// holding a visible view is the one displayed on its output. The second disjunct covers
// the empty displayed workspace, which has no view to carry the flag.
bool Workspaces::isWorkspaceVisible(const Json::Value& node) {
  return hasFlag(node, "visible") || (node["output"].isString() && isWorkspaceEmpty(node));
}

// Whether a workspace is in the state named by a state-keyed format-icons entry.
bool Workspaces::hasState(const Json::Value& node, const std::string& state) {
  if (state == "visible") {
    return isWorkspaceVisible(node);
  }
  if (state == "empty") {
    return isWorkspaceEmpty(node);
  }
  return hasFlag(node, state);
}

void Workspaces::updateWindows(const Json::Value& node, std::string& windows) {
  if ((node["type"].asString() == "con" || node["type"].asString() == "floating_con") &&
      node["name"].isString()) {
    std::string title = g_markup_escape_text(node["name"].asString().c_str(), -1);
    std::string windowClass = node["app_id"].isString()
                                  ? node["app_id"].asString()
                                  : node["window_properties"]["class"].asString();

    // Only add window rewrites that can be looked up
    if (!windowClass.empty()) {
      std::string windowReprKey = fmt::format("class<{}> title<{}>", windowClass, title);
      std::string window = m_windowRewriteRules.get(windowReprKey);
      // allow result to have formatting
      window = fmt::format(fmt::runtime(window), fmt::arg("name", title),
                           fmt::arg("class", windowClass));
      windows.append(window);
      windows.append(m_formatWindowSeparator);
    }
  }
  for (const Json::Value& child : node["nodes"]) {
    updateWindows(child, windows);
  }
  for (const Json::Value& child : node["floating_nodes"]) {
    updateWindows(child, windows);
  }
}

auto Workspaces::update() -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  bool needReorder = filterButtons();
  for (auto it = workspaces_.begin(); it != workspaces_.end(); ++it) {
    auto bit = buttons_.find((*it)["name"].asString());
    if (bit == buttons_.end()) {
      needReorder = true;
    }
    auto& ws = bit == buttons_.end() ? addButton(*it) : bit->second;
    auto& button = ws.button;
    if (needReorder) {
      box_.reorder_child(button, it - workspaces_.begin());
    }
    if (hasFlag((*it), "focused")) {
      button.get_style_context()->add_class("focused");
    } else {
      button.get_style_context()->remove_class("focused");
    }
    if (isWorkspaceVisible(*it)) {
      button.get_style_context()->add_class("visible");
    } else {
      button.get_style_context()->remove_class("visible");
    }
    if (hasFlag((*it), "urgent")) {
      button.get_style_context()->add_class("urgent");
    } else {
      button.get_style_context()->remove_class("urgent");
    }
    if ((*it)["target_output"].isString()) {
      button.get_style_context()->add_class("persistent");
    } else {
      button.get_style_context()->remove_class("persistent");
    }
    if (isWorkspaceEmpty(*it)) {
      button.get_style_context()->add_class("empty");
    } else {
      button.get_style_context()->remove_class("empty");
    }
    if ((*it)["output"].isString()) {
      // Simply attempt to remove all output classes every time to reset output classes. This works
      // even if a class has not been previously added to the style context.
      for (const auto& oclass : config_["output-classes"]) {
        button.get_style_context()->remove_class(oclass.asString());
      }
      // If output-classes contains a class for output associated with current workspace button, add
      // the class to its style context.
      std::string output_name = (*it)["output"].asString();
      if (config_["output-classes"].isMember(output_name) &&
          config_["output-classes"][output_name].isString()) {
        button.get_style_context()->add_class(config_["output-classes"][output_name].asString());
      }
      if (((*it)["output"].asString()) == bar_.output->name) {
        button.get_style_context()->add_class("current_output");
      } else {
        button.get_style_context()->remove_class("current_output");
      }
    } else {
      button.get_style_context()->remove_class("current_output");
    }
    std::string full_name;
    if (!config_["disable-markup"].asBool()) {
      full_name = g_markup_escape_text((*it)["name"].asString().c_str(), -1);
    } else {
      full_name = (*it)["name"].asString();
    }

    std::string windows = "";
    if (config_["window-rewrite"].isObject()) {
      updateWindows((*it), windows);
    }

    auto index = (*it)["num"].asInt();

    if (config_["format"].isString()) {
      std::string format;
      if (config_["format-for-negative-index"].isString() && index < 0) {
        format = config_["format-for-negative-index"].asString();
      } else {
        format = config_["format"].asString();
      }

      auto name = trimWorkspaceName(full_name);
      auto output = (*it)["output"].asString();
      auto icon = getIcon(full_name, *it);
      auto separated_windows =
          windows.substr(0, windows.length() - m_formatWindowSeparator.length());

      full_name =
          fmt::format(fmt::runtime(format), fmt::arg("index", index), fmt::arg("name", name),
                      fmt::arg("value", full_name), fmt::arg("output", output),
                      fmt::arg("icon", icon), fmt::arg("windows", separated_windows));
    }

    if (!config_["disable-markup"].asBool()) {
      ws.labelWidget().set_markup(full_name);
    } else {
      ws.labelWidget().set_text(full_name);
    }
    onButtonReady(*it, ws);
    if (taskbar_config_.enable) {
      updateWorkspaceTaskbar(ws, (*it)["name"].asString());
    }
  }
  // Call parent update
  AModule::update();
}

Workspaces::WorkspaceButton& Workspaces::addButton(const Json::Value& node) {
  auto pair = buttons_.emplace(
      std::piecewise_construct, std::forward_as_tuple(node["name"].asString()),
      std::forward_as_tuple(node["name"].asString(), taskbar_config_.enable, bar_.orientation));
  auto& ws = pair.first->second;
  auto& button = ws.button;
  box_.pack_start(button, false, false, 0);
  button.set_name("sway-workspace-" + node["name"].asString());
  button.set_relief(Gtk::RELIEF_NONE);
  if (!config_["disable-click"].asBool()) {
    button.signal_pressed().connect([this, node] {
      try {
        if (node["target_output"].isString()) {
          ipc_.sendCmd(IPC_COMMAND,
                       fmt::format(persistent_workspace_switch_cmd_, "--no-auto-back-and-forth",
                                   node["name"].asString(), node["target_output"].asString(),
                                   "--no-auto-back-and-forth", node["name"].asString()));
        } else {
          std::string flag =
              config_["disable-auto-back-and-forth"].asBool() ? "--no-auto-back-and-forth" : "";
          if (config_["no-switch-output"].asBool()) {
            ipc_.sendCmd(IPC_COMMAND,
                         fmt::format("[workspace=\"^{}$\"] move workspace to output current; "
                                     "workspace number {} \"{}\"",
                                     node["name"].asString(), flag, node["name"].asString()));
          } else if (node["num"].asInt() >= 0) {
            ipc_.sendCmd(IPC_COMMAND,
                         fmt::format(workspace_switch_number_cmd_, flag, node["num"].asInt()));
          } else {
            ipc_.sendCmd(IPC_COMMAND,
                         fmt::format(workspace_switch_cmd_, flag, node["name"].asString()));
          }
        }
      } catch (const std::exception& e) {
        spdlog::error("Workspaces: {}", e.what());
      }
    });
  }
  return ws;
}

std::string Workspaces::getIcon(const std::string& name, const Json::Value& node) {
  // "empty" ranks above "visible": sway only keeps an empty workspace in the tree while it
  // is the one displayed on its output, so every empty workspace is also visible and the
  // opposite order would make the "empty" icon unreachable.
  std::vector<std::string> keys = {
      "high-priority-named", "urgent", "focused", "empty", "visible", name, "default"};
  for (auto const& key : keys) {
    if (key == "high-priority-named") {
      auto it = std::find_if(high_priority_named_.begin(), high_priority_named_.end(),
                             [&](const std::string& member) { return member == name; });
      if (it != high_priority_named_.end()) {
        return config_["format-icons"][name].asString();
      }

      it = std::find_if(high_priority_named_.begin(), high_priority_named_.end(),
                        [&](const std::string& member) {
                          return trimWorkspaceName(member) == trimWorkspaceName(name);
                        });
      if (it != high_priority_named_.end()) {
        return config_["format-icons"][trimWorkspaceName(name)].asString();
      }
    }
    if (key == "focused" || key == "urgent" || key == "empty" || key == "visible") {
      if (config_["format-icons"][key].isString() && hasState(node, key)) {
        return config_["format-icons"][key].asString();
      }
    } else if (config_["format-icons"]["persistent"].isString() &&
               node["target_output"].isString()) {
      return config_["format-icons"]["persistent"].asString();
    } else if (config_["format-icons"][key].isString()) {
      return config_["format-icons"][key].asString();
    } else if (config_["format-icons"][trimWorkspaceName(key)].isString()) {
      return config_["format-icons"][trimWorkspaceName(key)].asString();
    }
  }
  return name;
}

bool Workspaces::handleScroll(GdkEventScroll* e) {
  if (gdk_event_get_pointer_emulated((GdkEvent*)e) != 0) {
    /**
     * Ignore emulated scroll events on window
     */
    return false;
  }
  auto dir = AModule::getScrollDir(e);
  if (dir == SCROLL_DIR::NONE) {
    return true;
  }
  std::string name;
  {
    bool alloutputs = config_["all-outputs"].asBool();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it =
        std::find_if(workspaces_.begin(), workspaces_.end(), [alloutputs](const auto& workspace) {
          if (alloutputs) {
            return hasFlag(workspace, "focused");
          }
          bool noNodes = workspace["nodes"].empty() && workspace["floating_nodes"].empty();
          return hasFlag(workspace, "visible") || (workspace["output"].isString() && noNodes);
        });
    if (it == workspaces_.end()) {
      return true;
    }
    bool reverse_scroll = config_["reverse-scroll"].isBool() && config_["reverse-scroll"].asBool();
    if (dir == SCROLL_DIR::DOWN || dir == SCROLL_DIR::RIGHT) {
      name = getCycleWorkspace(it, reverse_scroll ? true : false);
    } else if (dir == SCROLL_DIR::UP || dir == SCROLL_DIR::LEFT) {
      name = getCycleWorkspace(it, reverse_scroll ? false : true);
    } else {
      return true;
    }
    if (name == (*it)["name"].asString()) {
      return true;
    }
  }
  if (!config_["warp-on-scroll"].isNull() && !config_["warp-on-scroll"].asBool()) {
    ipc_.sendCmd(IPC_COMMAND, fmt::format("mouse_warping none"));
  }
  try {
    ipc_.sendCmd(IPC_COMMAND, fmt::format(workspace_switch_cmd_, "--no-auto-back-and-forth", name));
  } catch (const std::exception& e) {
    spdlog::error("Workspaces: {}", e.what());
  }
  if (!config_["warp-on-scroll"].isNull() && !config_["warp-on-scroll"].asBool()) {
    ipc_.sendCmd(IPC_COMMAND, fmt::format("mouse_warping container"));
  }
  return true;
}

std::string Workspaces::getCycleWorkspace(std::vector<Json::Value>::iterator it, bool prev) const {
  if (prev && it == workspaces_.begin() && !config_["disable-scroll-wraparound"].asBool()) {
    return (*(--workspaces_.end()))["name"].asString();
  }
  if (prev && it != workspaces_.begin())
    --it;
  else if (!prev && it != workspaces_.end())
    ++it;
  if (!prev && it == workspaces_.end()) {
    if (config_["disable-scroll-wraparound"].asBool()) {
      --it;
    } else {
      return (*(workspaces_.begin()))["name"].asString();
    }
  }
  return (*it)["name"].asString();
}

std::string Workspaces::trimWorkspaceName(const std::string& name) {
  std::size_t found = name.find(':');
  if (found != std::string::npos) {
    return name.substr(found + 1);
  }
  return name;
}

std::optional<uint16_t> Workspaces::getCustomSortIndex(const std::string& name) const {
  if (custom_sort_priorities_.empty()) {
    return std::nullopt;
  }
  auto it = custom_sort_priorities_.find(name);
  if (it != custom_sort_priorities_.end()) {
    return it->second;
  }
  auto trimmed = trimWorkspaceName(name);
  if (trimmed != name) {
    auto trimmed_it = custom_sort_priorities_.find(trimmed);
    if (trimmed_it != custom_sort_priorities_.end()) {
      return trimmed_it->second;
    }
  }
  return std::nullopt;
}

bool is_focused_recursive(const Json::Value& node) {
  // If a workspace has a focused container then get_tree will say
  // that the workspace itself isn't focused.  Therefore we need to
  // check if any of its nodes are focused as well.
  // some layouts like tabbed have many nested nodes
  // all nested nodes must be checked for focused flag
  if (node["focused"].asBool()) {
    return true;
  }

  for (const auto& child : node["nodes"]) {
    if (is_focused_recursive(child)) {
      return true;
    }
  }

  for (const auto& child : node["floating_nodes"]) {
    if (is_focused_recursive(child)) {
      return true;
    }
  }

  return false;
}

void Workspaces::onButtonReady(const Json::Value& node, WorkspaceButton& ws) {
  if (config_["current-only"].asBool()) {
    if (is_focused_recursive(node)) {
      ws.button.show();
    } else {
      ws.button.hide();
    }
  } else {
    ws.button.show();
  }
}

}  // namespace waybar::modules::sway
