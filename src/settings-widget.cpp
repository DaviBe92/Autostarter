#include "settings-widget.hpp"
#include "config.hpp"
#include "autostart.hpp"
#include "constants.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <QInputDialog>
#include <QDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <windows.h>
#include <shlobj.h>
#include <utility>

ProgramListItem::ProgramListItem(const QString &path, const Program &program,
				 QWidget *parent)
	: QWidget(parent)
{
	// Create layout for the program item row
	auto layout = new QHBoxLayout(this);
	layout->setContentsMargins(5, 0, 5, 0); // Reduce vertical margins

	QFileInfo fileInfo(path);
	pathLabel = new QLabel(fileInfo.fileName(), this);
	pathLabel->setToolTip(path);
	pathLabel->setAlignment(Qt::AlignVCenter);

	minimizedBox = new QCheckBox("Minimized", this);
	minimizedBox->setChecked(program.minimized);
	autocloseBox = new QCheckBox("Auto-close", this);
	autocloseBox->setChecked(program.autoclose);
	autocloseBox->setEnabled(fileInfo.suffix().isEmpty() ||
				 fileInfo.suffix().compare("exe", Qt::CaseInsensitive) == 0 ||
				 fileInfo.suffix().compare("lnk", Qt::CaseInsensitive) == 0);
	delayBox = new QSpinBox(this);
	delayBox->setRange(0, 3600);
	delayBox->setValue(program.delaySeconds);
	delayBox->setSuffix(" s");
	delayBox->setToolTip("Wait this long before starting this app");
	delayBox->setAccessibleName("Delay before launch");

	layout->addWidget(pathLabel);
	layout->addStretch();
	layout->addWidget(minimizedBox);
	layout->addWidget(autocloseBox);
	layout->addWidget(delayBox);
	argumentsButton = new QPushButton("Args…", this);
	argumentsButton->setEnabled(autocloseBox->isEnabled());
	argumentsButton->setToolTip(program.arguments.empty() ? "Set launch arguments" :
				    QString::fromStdString(program.arguments));
	layout->addWidget(argumentsButton);
}

SettingsWidget *settings_instance = nullptr;

SettingsWidget::SettingsWidget(QWidget *parent) : QWidget(parent)
{
	// Initialize and set up the main configuration window layout
	settings_instance = this;
	setWindowTitle(QString::fromStdString(Constants::WINDOW_TITLE));
	setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);

	auto mainLayout = new QVBoxLayout(this);

	enableCheckbox = new QCheckBox("Enable Autostarter", this);
	mainLayout->addWidget(enableCheckbox);

    mainLayout->addSpacing(10);

	loadoutLabel = new QLabel("Loadout:", this);
	mainLayout->addWidget(loadoutLabel);

	loadoutLayout = new QHBoxLayout();
	loadoutCombo = new QComboBox(this);
	addLoadoutButton = new QPushButton("Add", this);
	removeLoadoutButton = new QPushButton("Remove", this);
	addLoadoutButton->setMaximumWidth(60);
	removeLoadoutButton->setMaximumWidth(80);

	loadoutLayout->addWidget(loadoutCombo);
	loadoutLayout->addWidget(addLoadoutButton);
	loadoutLayout->addWidget(removeLoadoutButton);
	mainLayout->addLayout(loadoutLayout);

	auto &config = PluginConfig::Get();
	draft.loadouts = config.loadouts;
	for (const auto &loadout : draft.loadouts) {
		loadoutCombo->addItem(QString::fromStdString(loadout.name));
	}

	connect(loadoutCombo,
		QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&SettingsWidget::UpdateProgramList);
	connect(addLoadoutButton, &QPushButton::clicked, this,
		&SettingsWidget::onAddLoadoutClicked);
	connect(removeLoadoutButton, &QPushButton::clicked, this,
		&SettingsWidget::onRemoveLoadoutClicked);

	programsList = new QListWidget(this);
	mainLayout->addWidget(programsList);

	auto buttonLayout = new QHBoxLayout();
	addProgramButton = new QPushButton("Browse Files…", this);
	auto *startMenuButton = new QPushButton("Start Menu…", this);
	auto *moveUpButton = new QPushButton("Move Up", this);
	auto *moveDownButton = new QPushButton("Move Down", this);
	deleteProgramButton = new QPushButton("Remove", this);
	buttonLayout->addWidget(startMenuButton);
	buttonLayout->addWidget(addProgramButton);
	buttonLayout->addWidget(deleteProgramButton);
	buttonLayout->addWidget(moveUpButton);
	buttonLayout->addWidget(moveDownButton);
	mainLayout->addLayout(buttonLayout);

    mainLayout->addSpacing(10);

	auto checkboxLayout = new QHBoxLayout();
	askToLaunchCheckbox = new QCheckBox("Ask on launch", this);
	autocloseCheckbox = new QCheckBox("Auto-close on OBS exit", this);
	checkboxLayout->addWidget(askToLaunchCheckbox);
	checkboxLayout->addWidget(autocloseCheckbox);
	mainLayout->addLayout(checkboxLayout);

    mainLayout->addSpacing(10);

	auto launchLayout = new QHBoxLayout();
	launchButton = new QPushButton("Launch Apps", this);
	quitButton = new QPushButton("Quit Apps", this);
	launchLayout->addWidget(launchButton);
	launchLayout->addWidget(quitButton);
	mainLayout->addLayout(launchLayout);

	auto actionLayout = new QHBoxLayout();
	saveButton = new QPushButton("Save", this);
	closeButton = new QPushButton("Close", this);
	actionLayout->addWidget(saveButton);
	actionLayout->addWidget(closeButton);
	mainLayout->addLayout(actionLayout);

	mainLayout->addSpacing(10);

	versionLabel = new QLabel(
		"<a href=\"" + QString::fromStdString(Constants::GITHUB_URL) + "\">" + 
		QString::fromStdString(Constants::VERSION_STRING) + "</a>", this);
	versionLabel->setOpenExternalLinks(true);
	versionLabel->setCursor(Qt::PointingHandCursor);
	mainLayout->addWidget(versionLabel);

	connect(saveButton, &QPushButton::clicked, this,
		&SettingsWidget::onSave);
	connect(closeButton, &QPushButton::clicked, this,
		&SettingsWidget::onClose);
	connect(addProgramButton, &QPushButton::clicked, this,
		&SettingsWidget::onAddProgram);
	connect(startMenuButton, &QPushButton::clicked, this, &SettingsWidget::onAddStartMenuProgram);
	connect(moveUpButton, &QPushButton::clicked, this, [this] { moveProgram(-1); });
	connect(moveDownButton, &QPushButton::clicked, this, [this] { moveProgram(1); });
	connect(deleteProgramButton, &QPushButton::clicked, this,
		&SettingsWidget::onDeleteProgram);
	connect(launchButton, &QPushButton::clicked, this,
		&SettingsWidget::onLaunchApps);
	connect(quitButton, &QPushButton::clicked, this,
		&SettingsWidget::onQuitApps);

	// Load initial values from config
	enableCheckbox->setChecked(config.enabled);
	askToLaunchCheckbox->setChecked(config.askToLaunch);
	autocloseCheckbox->setChecked(config.autoclose);

	loadoutCombo->setCurrentText(QString::fromStdString(config.currentLoadout));
	UpdateProgramList();

	resize(Constants::DEFAULT_WINDOW_WIDTH,
	       Constants::DEFAULT_WINDOW_HEIGHT);
}

SettingsWidget::~SettingsWidget()
{
	settings_instance = nullptr;
}

void SettingsWidget::ShowSettings()
{
	if (!settings_instance) {
		auto widget = new SettingsWidget();
		widget->setAttribute(Qt::WA_DeleteOnClose);
		widget->show();
		widget->raise();
		widget->activateWindow();
	} else {
		settings_instance->show();
		settings_instance->raise();
		settings_instance->activateWindow();
	}
}

void SettingsWidget::onSave()
{
	draft.enabled = enableCheckbox->isChecked();
	draft.askToLaunch = askToLaunchCheckbox->isChecked();
	draft.autoclose = autocloseCheckbox->isChecked();
	draft.currentLoadout = loadoutCombo->currentText().toStdString();
	if (!draft.Save()) {
		QMessageBox::warning(this, "Autostarter", "The settings could not be saved.");
		return;
	}

	auto &config = PluginConfig::Get();
	config.enabled = draft.enabled;
	config.askToLaunch = draft.askToLaunch;
	config.autoclose = draft.autoclose;
	config.currentLoadout = std::move(draft.currentLoadout);
	config.loadouts = std::move(draft.loadouts);
	close();
}

void SettingsWidget::onClose()
{
	close();
}

void SettingsWidget::onAddProgram()
{
	addProgram(QFileDialog::getOpenFileName(this, "Select Program", "", "All Files (*.*)"));
}

void SettingsWidget::onAddStartMenuProgram()
{
	QDialog dialog(this);
	dialog.setWindowTitle("Add a Start Menu app");
	auto *layout = new QVBoxLayout(&dialog);
	auto *search = new QLineEdit(&dialog);
	search->setPlaceholderText("Search Start Menu shortcuts");
	auto *list = new QListWidget(&dialog);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(search);
	layout->addWidget(list);
	layout->addWidget(buttons);

	for (const auto *folder : {&FOLDERID_Programs, &FOLDERID_CommonPrograms}) {
		PWSTR folderPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(*folder, 0, nullptr, &folderPath))) {
			const auto root = QString::fromWCharArray(folderPath);
			QDirIterator entries(root, {"*.lnk", "*.url"}, QDir::Files | QDir::System,
					     QDirIterator::Subdirectories);
			while (entries.hasNext()) {
				const QFileInfo shortcut(entries.next());
				const auto relative = QDir(root).relativeFilePath(shortcut.absoluteFilePath());
				auto *item = new QListWidgetItem(relative.left(relative.lastIndexOf('.')), list);
				item->setData(Qt::UserRole, shortcut.absoluteFilePath());
				item->setToolTip(shortcut.absoluteFilePath());
			}
		}
		CoTaskMemFree(folderPath);
	}
	list->sortItems();
	auto *add = buttons->button(QDialogButtonBox::Ok);
	add->setEnabled(false);
	connect(list, &QListWidget::currentItemChanged, &dialog,
		[add](QListWidgetItem *item) { add->setEnabled(item && !item->isHidden()); });
	connect(search, &QLineEdit::textChanged, &dialog, [list, add](const QString &query) {
		for (int i = 0; i < list->count(); ++i)
			list->item(i)->setHidden(!list->item(i)->text().contains(query, Qt::CaseInsensitive));
		auto *item = list->currentItem();
		add->setEnabled(item && !item->isHidden());
	});
	connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.resize(520, 420);
	if (dialog.exec() == QDialog::Accepted && list->currentItem())
		addProgram(list->currentItem()->data(Qt::UserRole).toString());
}

void SettingsWidget::addProgram(const QString &filename)
{
	if (filename.isEmpty())
		return;

	const QFileInfo file(filename);
	if (!file.isFile() || !file.isReadable()) {
		QMessageBox::warning(this, "Autostarter", "The selected file is missing or unreadable.");
		return;
	}

	if (auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString())) {
		Program program;
		program.path = file.absolutePath().toStdString();
		program.executable = file.fileName().toStdString();
		loadout->programs.push_back(std::move(program));
		UpdateProgramList();
		programsList->setCurrentRow(programsList->count() - 1);
	}
}

void SettingsWidget::onDeleteProgram()
{
	const int row = programsList->currentRow();
	auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString());
	if (loadout && row >= 0) {
		loadout->programs.erase(loadout->programs.begin() + row);
		UpdateProgramList();
		programsList->setCurrentRow(qMin(row, programsList->count() - 1));
	}
}

void SettingsWidget::moveProgram(int offset)
{
	const int row = programsList->currentRow();
	const int destination = row + offset;
	auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString());
	if (loadout && row >= 0 && destination >= 0 && destination < programsList->count()) {
		std::swap(loadout->programs[row], loadout->programs[destination]);
		UpdateProgramList();
		programsList->setCurrentRow(destination);
	}
}

void SettingsWidget::onLaunchApps()
{
	if (const auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString()))
		AutoStarter::LaunchPrograms(*loadout);
}

void SettingsWidget::onQuitApps()
{
	// Trigger quitting launched apps
	AutoStarter::QuitPrograms();
}

void SettingsWidget::UpdateProgramList()
{
	programsList->clear();
	auto &config = draft;
	if (Loadout *loadout = config.GetLoadout(
		    loadoutCombo->currentText().toStdString())) {
		for (size_t index = 0; index < loadout->programs.size(); ++index) {
			const auto &program = loadout->programs[index];
			QString fullPath = QString::fromStdString(
				program.path + "/" + program.executable);
			auto item = new QListWidgetItem(programsList);
			auto widget = new ProgramListItem(
				fullPath, program, programsList);
			item->setSizeHint(QSize(item->sizeHint().width(),
						30)); // Set consistent height
			programsList->setItemWidget(item, widget);

			const auto editProgram = [this, name = loadout->name, index]() -> Program & {
				return draft.GetLoadout(name)->programs[index];
			};
			connect(widget->minimizedBox, &QCheckBox::toggled, widget,
				[editProgram](bool value) { editProgram().minimized = value; });
			connect(widget->autocloseBox, &QCheckBox::toggled, widget,
				[editProgram](bool value) { editProgram().autoclose = value; });
			connect(widget->delayBox, &QSpinBox::valueChanged, widget,
				[editProgram](int value) { editProgram().delaySeconds = value; });
			connect(widget->argumentsButton, &QPushButton::clicked, widget, [widget, editProgram] {
				bool accepted = false;
				const auto args = QInputDialog::getText(widget, "Launch arguments",
					"Arguments (for example, --profile \"Streaming\")",
					QLineEdit::Normal, QString::fromStdString(editProgram().arguments), &accepted);
				if (accepted) {
					editProgram().arguments = args.toStdString();
					widget->argumentsButton->setToolTip(args.isEmpty() ? "Set launch arguments" : args);
				}
			});
		}
	}
}

void SettingsWidget::onAddLoadoutClicked()
{
	QDialog dialog(this);
	dialog.setWindowTitle("Add new Loadout");
	dialog.setModal(true);

	auto layout = new QVBoxLayout(&dialog);
	auto nameField = new QLineEdit(&dialog);
	auto label = new QLabel("Enter loadout name:", &dialog);
	auto errorLabel = new QLabel(&dialog);
	errorLabel->setStyleSheet("color: red;");
	errorLabel->hide();

	layout->addWidget(label);
	layout->addWidget(nameField);
	layout->addWidget(errorLabel);

	auto buttonBox = new QHBoxLayout();
	auto addButton = new QPushButton("Add", &dialog);
	auto cancelButton = new QPushButton("Cancel", &dialog);
	buttonBox->addWidget(addButton);
	buttonBox->addWidget(cancelButton);
	layout->addLayout(buttonBox);

	connect(nameField, &QLineEdit::returnPressed, addButton,
		&QPushButton::click);

	connect(addButton, &QPushButton::clicked, &dialog, [&]() {
		if (nameField->text().isEmpty()) {
			errorLabel->setText("Name cannot be empty");
			errorLabel->show();
			return;
		}

		auto &config = draft;
		if (config.AddLoadout(nameField->text().toStdString())) {
			loadoutCombo->addItem(nameField->text());
			loadoutCombo->setCurrentText(nameField->text());
			dialog.accept();
		} else {
			errorLabel->setText(
				"A loadout with this name already exists");
			errorLabel->show();
		}
	});

	connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

	dialog.exec();
}

void SettingsWidget::onRemoveLoadoutClicked()
{
	QString currentLoadout = loadoutCombo->currentText();
	if (currentLoadout.isEmpty())
		return;

	QMessageBox confirm(this);
	confirm.setWindowTitle("Remove Loadout");
	confirm.setText("Do you really want to remove the current loadout?");
	confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
	confirm.setDefaultButton(QMessageBox::Cancel);

	if (confirm.exec() == QMessageBox::Yes) {
		auto &config = draft;
		config.RemoveLoadout(currentLoadout.toStdString());
		loadoutCombo->removeItem(loadoutCombo->currentIndex());
		// If no loadouts left, create a default one
		if (config.loadouts.empty()) {
			config.InitDefaultLoadout();
			loadoutCombo->addItem(QString::fromStdString(
				config.loadouts[0].name));
		}
	}
}
