#include "config.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

/**
 * @brief Returns the singleton instance of PluginConfig
 */
PluginConfig& PluginConfig::Get()
{
    static PluginConfig instance;
    return instance;
}

/**
 * @brief Constructs the path to the config file
 * @return QString containing the full path to config.json
 */
QString PluginConfig::GetConfigPath()
{
	char *config_dir = obs_module_config_path("");
	if (!config_dir)
		return QString();

	QString path = QString::fromUtf8(config_dir);
	bfree(config_dir);

	return QDir(path).filePath("config.json");
}

/**
 * @brief Serializes the configuration to JSON format
 * @return QJsonObject containing all configuration data
 */
QJsonObject PluginConfig::ToJson() const
{
	// Create root JSON object
	QJsonObject json;
	json["enabled"] = enabled;
	json["currentLoadout"] = QString::fromStdString(currentLoadout);
	json["askToLaunch"] = askToLaunch;
	json["autoclose"] = autoclose;

	// Serialize loadouts array
	QJsonArray loadoutsArray;
	for (const auto &loadout : loadouts) {
		// Create loadout object
		QJsonObject loadoutObj;
		loadoutObj["name"] = QString::fromStdString(loadout.name);

		// Serialize programs in loadout
		QJsonArray programsArray;
		for (const auto &program : loadout.programs) {
			QJsonObject programObj;
			programObj["path"] =
				QString::fromStdString(program.path);
			programObj["executable"] =
				QString::fromStdString(program.executable);
			programObj["minimized"] = program.minimized;
			programObj["autoclose"] = program.autoclose;
			programObj["delaySeconds"] = program.delaySeconds;
			programObj["arguments"] = QString::fromStdString(program.arguments);
			programsArray.append(programObj);
		}
		loadoutObj["programs"] = programsArray;

		loadoutsArray.append(loadoutObj);
	}
	json["loadouts"] = loadoutsArray;

	return json;
}

/**
 * @brief Deserializes configuration from JSON format
 * @param json QJsonObject containing configuration data
 */
void PluginConfig::FromJson(const QJsonObject &json)
{
	// Load basic settings
	enabled = json["enabled"].toBool(false);
	currentLoadout = json["currentLoadout"].toString().toStdString();
	askToLaunch = json["askToLaunch"].toBool(true);
	autoclose = json["autoclose"].toBool(false);

	// Parse loadouts array
	loadouts.clear();
	QJsonArray loadoutsArray = json["loadouts"].toArray();
	for (const auto &loadoutVal : loadoutsArray) {
		QJsonObject loadoutObj = loadoutVal.toObject();
		Loadout loadout;
		loadout.name = loadoutObj["name"].toString().toStdString();

		QJsonArray programsArray = loadoutObj["programs"].toArray();
		for (const auto &programVal : programsArray) {
			QJsonObject programObj = programVal.toObject();
			Program program;
			program.path =
				programObj["path"].toString().toStdString();
			program.executable = programObj["executable"]
						     .toString()
						     .toStdString();
			program.minimized = programObj["minimized"].toBool(false);
			program.autoclose = programObj["autoclose"].toBool(true);
			program.delaySeconds = std::clamp(programObj["delaySeconds"].toInt(), 0, 3600);
			program.arguments = programObj["arguments"].toString().toStdString();
			loadout.programs.push_back(program);
		}

		loadouts.push_back(loadout);
	}
}

bool PluginConfig::Save()
{
	QString configPath = GetConfigPath();
	QSaveFile file(configPath);
	const auto data = QJsonDocument(ToJson()).toJson(QJsonDocument::Indented);
	if (configPath.isEmpty() || !QDir().mkpath(QFileInfo(configPath).absolutePath()) ||
	    !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		blog(LOG_WARNING, "[Autostarter] Could not save configuration");
		return false;
	}
	return true;
}

void PluginConfig::Load()
{
	QString configPath = GetConfigPath();
	QFile file(configPath);
	if (!configPath.isEmpty() && file.exists()) {
		const auto doc = file.open(QIODevice::ReadOnly)
					 ? QJsonDocument::fromJson(file.readAll())
					 : QJsonDocument();
		if (doc.isObject())
			FromJson(doc.object());
		else
			blog(LOG_WARNING, "[Autostarter] Could not read configuration");
	}
	if (loadouts.empty())
		InitDefaultLoadout();
	if (!GetLoadout(currentLoadout))
		currentLoadout = loadouts.front().name;
}

bool PluginConfig::AddLoadout(const std::string &name)
{
	Loadout loadout;
	loadout.name = name;
	// Check if loadout with the same name already exists
	if (std::find_if(loadouts.begin(), loadouts.end(),
			 [&name](const Loadout &l) {
				 return l.name == name;
			 }) != loadouts.end()) {
		return false;
	}
	loadouts.push_back(loadout);
	return true;
}

void PluginConfig::RemoveLoadout(const std::string &name)
{
	loadouts.erase(std::remove_if(loadouts.begin(), loadouts.end(),
				      [&name](const Loadout &l) {
					      return l.name == name;
				      }),
		       loadouts.end());
}

Loadout *PluginConfig::GetLoadout(const std::string &name)
{
	auto it = std::find_if(loadouts.begin(), loadouts.end(),
			       [&name](const Loadout &l) {
				       return l.name == name;
			       });
	return it != loadouts.end() ? &(*it) : nullptr;
}

void PluginConfig::InitDefaultLoadout()
{
	Loadout loadout;
	loadout.name = "Default";
	loadouts.push_back(loadout);
}
