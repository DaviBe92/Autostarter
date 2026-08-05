#pragma once
#include <QtWidgets/QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>

class ProgramListItem : public QWidget {
	Q_OBJECT
public:
	/**
	 * @brief Constructs a single program item widget.
	 * @param path Full path (directory + filename).
	 * @param minimized Whether program should launch minimized.
	 * @param isRelative Whether the path is relative.
	 * @param parent Parent widget pointer.
	 */
	ProgramListItem(const QString &path, bool minimized, bool isRelative = false, QWidget *parent = nullptr);
	QString getPath() const { return fullPath; }
	bool isMinimized() const { return minimizedBox->isChecked(); }
	bool isRelativePath() const { return relativeFlag; }

private:
	QString fullPath;
	bool relativeFlag;
	/**
	 * @brief Label that displays the program's name or filename.
	 */
	QLabel *pathLabel;
	/**
	 * @brief Checkbox indicating if the program should be launched minimized.
	 */
	QCheckBox *minimizedBox;
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
	QLabel *titleLabel;
	QCheckBox *enableCheckbox;
	QComboBox *loadoutCombo;
	QListWidget *programsList;
	QPushButton *addProgramButton;
	QPushButton *deleteProgramButton;
	QPushButton *addRelativeProgramButton;
	QLineEdit *relativePathInput;
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
	/**
	 * @brief Add a new program with relative path to the currently selected loadout.
	 */
	void onAddRelativePath();
	void onDeleteProgram();
	void onLaunchApps();
	void onQuitApps();
	void onAddLoadoutClicked();
	void onRemoveLoadoutClicked();
};

extern SettingsWidget *settings_instance;
