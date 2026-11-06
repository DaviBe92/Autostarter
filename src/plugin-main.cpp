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
#include <QMessageBox>

OBS_DECLARE_MODULE()

// Close launched apps when the OBS UI exits. OBS 33 restricts obs_module_unload
// to saving data and releasing references.
static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && PluginConfig::Get().autoclose)
		AutoStarter::QuitPrograms();
}

/**
 * @brief Initializes the Autostarter plugin and handles loadout launching
 * 
 * This function:
 * 1. Checks for command line arguments (--autostarter <loadout>)
 * 2. Sets up the Tools menu integration
 * 3. Loads plugin configuration
 * 4. Launches applications based on:
 *    - Command line loadout (highest priority)
 *    - Auto-launch settings (if enabled)
 *    - User prompt (if askToLaunch is enabled)
 * 
 * @return true if initialization is successful
 */
bool obs_module_load(void)
{

	struct obs_cmdline_args cmdargs = obs_get_cmdline_args();

	// Look for our custom argument | --autostarter <"loadout"> / This overrides the enabled and askToLaunch check
	std::string cmdLoadout;
	for (int i = 1; i < cmdargs.argc; i++) {
		std::string arg = cmdargs.argv[i];
		if (arg == "--autostarter" && i + 1 < cmdargs.argc) {
			cmdLoadout = cmdargs.argv[i + 1];
			break;
		}
	}

	// Add menu item to existing Tools menu
	obs_frontend_add_tools_menu_item(
		"Autostarter", [](void *) { SettingsWidget::ShowSettings(); },
		nullptr);

	PluginConfig::Get().Load();
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	// Check if loadout was specified via command line
	if (!cmdLoadout.empty()) {
		// Check if the given loadout exists
		if (!PluginConfig::Get().GetLoadout(cmdLoadout)) {
			std::string errorString =
				"Loadout '" + cmdLoadout + "' not found";
			// Show an error modal
			QMessageBox msgBox;
			msgBox.setIcon(QMessageBox::Warning);
			msgBox.setWindowTitle("Autostarter");
			msgBox.setText(errorString.c_str());
			msgBox.setStandardButtons(QMessageBox::Ok);
			msgBox.setDefaultButton(QMessageBox::Ok);
			msgBox.exec();

		} else {
			// Launch the applications provided by the given loadout
			AutoStarter::LaunchPrograms(cmdLoadout.c_str());
		}
	} else {
		// Check if the plugin is enabled
		if (PluginConfig::Get().enabled) {
			// Check if the plugin should ask to launch
			if (PluginConfig::Get().askToLaunch) {
				launch_widget_create();
			} else {
				// Launch the applications of the current loadout
				AutoStarter::LaunchPrograms(
					PluginConfig::Get().currentLoadout);
			}
		}
	}
	return true;
}

/**
 * @brief Unregisters the frontend callback. Process termination happens on
 * OBS_FRONTEND_EVENT_EXIT (see on_frontend_event).
 */
void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
}
