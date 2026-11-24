/*
 * Autostarter Plugin for OBS Studio
 * Automatically launches configured applications when OBS starts
 * 
 * Features:
 * - Command line support for loadouts via --autostarter argument
 * - Configurable auto-launch behavior
 * - Multiple loadout support
 * - Optional process termination on OBS shutdown
 * 
 * Plugin Name
 * Copyright (C) <2024> <DaviBe> <davi.be92@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "launch-widget.hpp"
#include "settings-widget.hpp"
#include "config.hpp"
#include "autostart.hpp"
#include <QElapsedTimer>
#include <QMessageBox>
#include <QTimer>
#include <memory>
#include <string_view>

OBS_DECLARE_MODULE()

namespace {
std::unique_ptr<QObject> startupContext;
std::string commandLineLoadout;
bool frontendCallbackRegistered = false;

void launchAtStartup()
{
	auto &config = PluginConfig::Get();
	if (!commandLineLoadout.empty()) {
		if (config.GetLoadout(commandLineLoadout)) {
			AutoStarter::LaunchPrograms(commandLineLoadout);
		} else {
			auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
			auto *message = new QMessageBox(QMessageBox::Warning, "Autostarter",
				QString("Loadout '%1' was not found.").arg(QString::fromStdString(commandLineLoadout)),
				QMessageBox::Ok, mainWindow);
			message->setAttribute(Qt::WA_DeleteOnClose);
			message->open();
		}
	} else if (config.enabled) {
		if (config.askToLaunch)
			launch_widget_create();
		else
			AutoStarter::LaunchPrograms();
	}
}

void onFrontendEvent(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(0, startupContext.get(), launchAtStartup);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
		frontendCallbackRegistered = false;
		startupContext.reset();
		launch_widget_destroy();
		delete settings_instance;
		AutoStarter::QuitPrograms(true);
	}
}
} // namespace

bool obs_module_load()
{
	QElapsedTimer timer;
	timer.start();
	const auto args = obs_get_cmdline_args();
	for (int i = 1; i + 1 < args.argc; ++i) {
		if (std::string_view(args.argv[i]) == "--autostarter") {
			commandLineLoadout = args.argv[i + 1];
			break;
		}
	}

	PluginConfig::Get().Load();
	obs_frontend_add_tools_menu_item(
		"Autostarter", [](void *) { SettingsWidget::ShowSettings(); }, nullptr);

	startupContext = std::make_unique<QObject>();
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	frontendCallbackRegistered = true;
	blog(LOG_INFO, "[Autostarter] Module initialized in %lld ms", static_cast<long long>(timer.elapsed()));
	return true;
}

void obs_module_unload()
{
	if (frontendCallbackRegistered) {
		obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
		frontendCallbackRegistered = false;
	}
	startupContext.reset();
	launch_widget_destroy();
	delete settings_instance;
	AutoStarter::QuitPrograms(true);
	commandLineLoadout.clear();
}
