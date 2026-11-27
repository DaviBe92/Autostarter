#pragma once
#include <QtWidgets/QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>
#include "config.hpp"

#include <QToolButton>
#include <QCloseEvent>

class QSpinBox;

class ProgramListItem : public QWidget {
	Q_OBJECT
public:
	/**
	 * @brief Constructs a single program item widget.
	 * @param path Full path (directory + filename).
	 * @param program Launch and shutdown options.
	 * @param parent Parent widget pointer.
	 */
	ProgramListItem(const QString &path, const Program &program, QWidget *parent = nullptr);
	QCheckBox *minimizedBox;
	QCheckBox *autocloseBox;
	QSpinBox *delayBox;
	QPushButton *argumentsButton;
	QToolButton *upButton;
	QToolButton *downButton;
	QToolButton *removeButton;

private:
	/**
	 * @brief Application icon shown at the left of the row.
	 */
	QLabel *iconLabel;
	/**
	 * @brief Label that displays the program's name.
	 */
	QLabel *pathLabel;
};

/**
 * @brief Main settings interface for managing loadouts and plugin configuration.
 */
class SettingsWidget : public QWidget {
	Q_OBJECT
public:
	/**
	 * @brief Constructs the settings dialog.
	 * @param parent Parent widget pointer.
	 */
	explicit SettingsWidget(QWidget *parent = nullptr);
	/**
	 * @brief Destructor to clean up resources.
	 */
	~SettingsWidget();
	/**
	 * @brief Shows settings as a singleton widget.
	 */
	static void ShowSettings();

private:
	PluginConfig draft;
	QCheckBox *enableCheckbox;
	QComboBox *loadoutCombo;
	QListWidget *programsList;
	QPushButton *addProgramButton;
	QCheckBox *askToLaunchCheckbox;
	QCheckBox *autocloseCheckbox;
	QPushButton *saveButton;
	QPushButton *closeButton;
	QPushButton *launchButton;
	QLabel *versionLabel;
	QPushButton *quitButton;
	QLabel *socialLabel;
	QLabel *githubLink;
	QLabel *discordLink;
	QLabel *loadoutLabel;
	QHBoxLayout *loadoutLayout;
	QPushButton *addLoadoutButton;
	QPushButton *removeLoadoutButton;

	/**
	 * @brief Updates the program list according to the selected loadout.
	 */
	void UpdateProgramList();
	void addProgram(const QString &filename);
	void addShellApp(const QString &id);
	void removeProgram(int row);
	void moveProgram(int row, int offset);
	void onAddStartMenuProgram();
	/**
	 * @brief Writes the draft to disk and applies it. Returns false if saving failed.
	 */
	bool saveSettings();
	/**
	 * @brief Describes everything the user can change, used to detect unsaved changes.
	 */
	QString snapshot() const;
	QString savedSnapshot;

protected:
	void closeEvent(QCloseEvent *event) override;

private slots:
	/**
	 * @brief Save changes and close the settings.
	 */
	void onSave();
	/**
	 * @brief Close the settings window without saving.
	 */
	void onClose();
	/**
	 * @brief Add a new program to the currently selected loadout.
	 */
	void onAddProgram();
	void onLaunchApps();
	void onQuitApps();
	void onAddLoadoutClicked();
	void onRemoveLoadoutClicked();
};

extern SettingsWidget *settings_instance;
