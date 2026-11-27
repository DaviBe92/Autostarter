#include "settings-widget.hpp"
#include "config.hpp"
#include "autostart.hpp"
#include "constants.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileIconProvider>
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
#include <QToolButton>
#include <QTimer>
#include <QSet>
#include <QPixmap>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <obs-module.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <utility>

namespace {
const QString kShellAppsPath = "shell:AppsFolder";
constexpr int kIconSize = 24;

using Microsoft::WRL::ComPtr;

QImage ImageFromBitmap(HBITMAP bitmap)
{
	BITMAP info{};
	if (!GetObject(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0)
		return {};

	BITMAPINFO header{};
	header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	header.bmiHeader.biWidth = info.bmWidth;
	header.bmiHeader.biHeight = -info.bmHeight; // top-down
	header.bmiHeader.biPlanes = 1;
	header.bmiHeader.biBitCount = 32;
	header.bmiHeader.biCompression = BI_RGB;

	QImage image(info.bmWidth, info.bmHeight, QImage::Format_ARGB32_Premultiplied);
	HDC dc = GetDC(nullptr);
	const int lines = GetDIBits(dc, bitmap, 0, info.bmHeight, image.bits(), &header, DIB_RGB_COLORS);
	ReleaseDC(nullptr, dc);
	return lines ? image : QImage();
}

struct ShellApp {
	QString name;
	QIcon icon;
};

// Looks up a Start menu app (including Microsoft Store apps) by its AppsFolder id.
ShellApp LoadShellApp(const QString &id)
{
	ShellApp app;
	ComPtr<IShellItem> item;
	const std::wstring target = L"shell:AppsFolder\\" + id.toStdWString();
	if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item))))
		return app;

	PWSTR name = nullptr;
	if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
		app.name = QString::fromWCharArray(name);
		CoTaskMemFree(name);
	}

	ComPtr<IShellItemImageFactory> factory;
	if (SUCCEEDED(item.As(&factory))) {
		HBITMAP bitmap = nullptr;
		const SIZE size{kIconSize * 2, kIconSize * 2};
		if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap) {
			const QImage image = ImageFromBitmap(bitmap);
			DeleteObject(bitmap);
			if (!image.isNull())
				app.icon = QIcon(QPixmap::fromImage(image));
		}
	}
	return app;
}

struct StoreApp {
	QString name;
	QString id;
};

// Packaged (Microsoft Store) apps have no Start menu shortcut file, so they have to be
// read from the shell's Apps folder. Their ids look like "PackageFamilyName!AppId".
QList<StoreApp> EnumeratePackagedApps()
{
	QList<StoreApp> apps;
	PIDLIST_ABSOLUTE pidl = nullptr;
	if (FAILED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &pidl)))
		return apps;

	ComPtr<IShellItem> folder;
	const HRESULT created = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&folder));
	CoTaskMemFree(pidl);
	if (FAILED(created))
		return apps;

	ComPtr<IEnumShellItems> entries;
	if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&entries))))
		return apps;

	ComPtr<IShellItem> item;
	ULONG fetched = 0;
	while (entries->Next(1, &item, &fetched) == S_OK && fetched == 1) {
		PWSTR name = nullptr;
		PWSTR id = nullptr;
		if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
		    SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id))) {
			const auto appId = QString::fromWCharArray(id);
			if (appId.contains('!'))
				apps.push_back({QString::fromWCharArray(name), appId});
		}
		CoTaskMemFree(name);
		CoTaskMemFree(id);
		item.Reset();
	}
	return apps;
}

// Start menu folders are full of uninstallers, manuals and web links. Hide those by default.
bool LooksLikeNotAnApp(const QString &name)
{
	static const QStringList markers = {
		"uninstall", "unins ",   "readme",    "read me",  "release notes", "license",
		"licence",   "eula",     "documentation", "manual", "user guide",   "help",
		"faq",       "on the web", "website",   "web site", "sample",        "reference",
		"tutorial",  "what's new", "changelog"};
	const auto lower = name.toLower();
	for (const auto &marker : markers) {
		if (lower.contains(marker))
			return true;
	}
	return false;
}

QString DisplayName(const QString &fileName)
{
	const QFileInfo info(fileName);
	const auto suffix = info.suffix().toLower();
	if (suffix == "lnk" || suffix == "url" || suffix == "exe")
		return info.completeBaseName();
	return fileName;
}

QIcon FileIcon(const QString &fullPath)
{
	QFileInfo info(fullPath);
	if (info.suffix().compare("lnk", Qt::CaseInsensitive) == 0) {
		const QFileInfo target(info.symLinkTarget());
		if (target.exists())
			info = target;
	}
	return QFileIconProvider().icon(info);
}

struct AppCandidate {
	QString name;
	QString file; // Start menu shortcut path, empty for Store apps
	QString id;   // Apps folder id, only for Store apps
	bool notAnApp;
};

// Reads the Start menu folders and the Store app list. Slow, so it runs on a worker thread.
QList<AppCandidate> ScanApps()
{
	QElapsedTimer timer;
	timer.start();
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	QList<AppCandidate> candidates;
	QSet<QString> seen;
	for (const auto *folder : {&FOLDERID_Programs, &FOLDERID_CommonPrograms}) {
		PWSTR folderPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(*folder, 0, nullptr, &folderPath))) {
			const auto root = QString::fromWCharArray(folderPath);
			QDirIterator entries(root, {"*.lnk", "*.url"}, QDir::Files | QDir::System,
					     QDirIterator::Subdirectories);
			while (entries.hasNext()) {
				const QFileInfo shortcut(entries.next());
				const auto name = shortcut.completeBaseName();
				if (seen.contains(name.toLower()))
					continue;
				seen.insert(name.toLower());
				const bool webLink = shortcut.suffix().compare("url", Qt::CaseInsensitive) == 0;
				candidates.push_back({name, shortcut.absoluteFilePath(), {},
						      webLink || LooksLikeNotAnApp(name)});
			}
		}
		CoTaskMemFree(folderPath);
	}
	for (const auto &app : EnumeratePackagedApps()) {
		if (seen.contains(app.name.toLower()))
			continue;
		seen.insert(app.name.toLower());
		candidates.push_back({app.name, {}, app.id, LooksLikeNotAnApp(app.name)});
	}
	std::sort(candidates.begin(), candidates.end(), [](const AppCandidate &a, const AppCandidate &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});

	if (SUCCEEDED(com))
		CoUninitialize();
	blog(LOG_INFO, "[Autostarter] Found %lld apps in %lld ms", static_cast<long long>(candidates.size()),
	     static_cast<long long>(timer.elapsed()));
	return candidates;
}

std::shared_future<QList<AppCandidate>> appScan;
std::chrono::steady_clock::time_point appScanStarted;

void StartAppScan()
{
	appScanStarted = std::chrono::steady_clock::now();
	appScan = std::async(std::launch::async, ScanApps).share();
}
} // namespace

ProgramListItem::ProgramListItem(const QString &path, const Program &program,
				 QWidget *parent)
	: QWidget(parent)
{
	const bool shellApp = QString::fromStdString(program.path) == kShellAppsPath;

	auto layout = new QHBoxLayout(this);
	layout->setContentsMargins(6, 4, 6, 4);
	layout->setSpacing(8);

	// Application icon and name
	QString displayName;
	QIcon icon;
	if (shellApp) {
		const auto app = LoadShellApp(QString::fromStdString(program.executable));
		displayName = app.name.isEmpty() ? QString::fromStdString(program.executable) : app.name;
		icon = app.icon;
	} else {
		displayName = DisplayName(QFileInfo(path).fileName());
		icon = FileIcon(path);
	}

	iconLabel = new QLabel(this);
	iconLabel->setFixedSize(kIconSize, kIconSize);
	iconLabel->setScaledContents(true);
	if (!icon.isNull())
		iconLabel->setPixmap(icon.pixmap(kIconSize * 2, kIconSize * 2));

	pathLabel = new QLabel(displayName, this);
	pathLabel->setToolTip(shellApp ? displayName + "\nDrag to reorder" : path + "\nDrag to reorder");
	pathLabel->setMinimumWidth(80);
	pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	// Right-to-left layout puts each label to the left of its box, like "Delay".
	minimizedBox = new QCheckBox("Minimized", this);
	minimizedBox->setLayoutDirection(Qt::RightToLeft);
	minimizedBox->setChecked(program.minimized);

	const auto suffix = QFileInfo(path).suffix();
	const bool trackable = !shellApp && (suffix.isEmpty() || suffix.compare("exe", Qt::CaseInsensitive) == 0 ||
					     suffix.compare("lnk", Qt::CaseInsensitive) == 0);
	autocloseBox = new QCheckBox("Auto-close", this);
	autocloseBox->setLayoutDirection(Qt::RightToLeft);
	const bool closable = trackable || shellApp; // Store apps are tracked by process id
	autocloseBox->setChecked(program.autoclose && closable);
	autocloseBox->setEnabled(closable);
	if (!closable)
		autocloseBox->setToolTip("Auto-close is not available for this type of app");

	auto *delayLabel = new QLabel("Delay", this);
	delayBox = new QSpinBox(this);
	delayBox->setRange(0, 3600);
	delayBox->setValue(program.delaySeconds);
	delayBox->setSuffix(" s");
	delayBox->setFixedWidth(80);
	delayBox->setToolTip("Wait this long before starting this app");
	delayBox->setAccessibleName("Delay before launch");

	argumentsButton = new QPushButton("Args", this);
	argumentsButton->setMinimumWidth(64);
	argumentsButton->setEnabled(trackable);
	argumentsButton->setToolTip(!trackable ? "Launch arguments are not available for this type of app" :
			    program.arguments.empty() ? "Set launch arguments" :
							QString::fromStdString(program.arguments));

	const auto makeToolButton = [this](const QString &text, const QString &tip) {
		auto *button = new QToolButton(this);
		button->setText(text);
		button->setToolTip(tip);
		button->setFixedSize(30, 30);
		button->setStyleSheet("QToolButton { padding: 0px; }");
		return button;
	};
	upButton = makeToolButton("↑", "Move up");
	downButton = makeToolButton("↓", "Move down");
	removeButton = makeToolButton("✕", "Remove from list");

	layout->addWidget(iconLabel);
	layout->addWidget(pathLabel, 1);
	layout->addWidget(minimizedBox);
	layout->addWidget(autocloseBox);
	layout->addWidget(delayLabel);
	layout->addWidget(delayBox);
	layout->addWidget(argumentsButton);
	layout->addSpacing(4);
	layout->addWidget(upButton);
	layout->addWidget(downButton);
	layout->addWidget(removeButton);
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

	loadoutLabel = new QLabel("Loadout:", this);
	loadoutLayout = new QHBoxLayout();
	loadoutLayout->addWidget(loadoutLabel);
	loadoutCombo = new QComboBox(this);
	addLoadoutButton = new QPushButton("Add", this);
	removeLoadoutButton = new QPushButton("Remove", this);
	addLoadoutButton->setMinimumWidth(80);
	removeLoadoutButton->setMinimumWidth(80);

	loadoutLayout->addWidget(loadoutCombo, 1);
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
	programsList->setSelectionMode(QAbstractItemView::SingleSelection);
	programsList->setDragDropMode(QAbstractItemView::InternalMove);
	programsList->setDefaultDropAction(Qt::MoveAction);
	programsList->setDragEnabled(true);
	mainLayout->addWidget(programsList);

	// Dragging a row reorders the loadout. Item widgets don't survive a move, so the list is rebuilt.
	connect(programsList->model(), &QAbstractItemModel::rowsMoved, this,
		[this](const QModelIndex &, int start, int, const QModelIndex &, int destination) {
			auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString());
			if (!loadout || start < 0 || start >= static_cast<int>(loadout->programs.size()))
				return;
			const int target = destination > start ? destination - 1 : destination;
			auto program = std::move(loadout->programs[start]);
			loadout->programs.erase(loadout->programs.begin() + start);
			loadout->programs.insert(loadout->programs.begin() +
							 qBound(0, target, static_cast<int>(loadout->programs.size())),
						 std::move(program));
			QTimer::singleShot(0, this, [this, target] {
				UpdateProgramList();
				programsList->setCurrentRow(qBound(0, target, programsList->count() - 1));
			});
		});

	auto buttonLayout = new QHBoxLayout();
	addProgramButton = new QPushButton("Browse Files", this);
	auto *addAppButton = new QPushButton("Add App", this);
	buttonLayout->addWidget(addAppButton);
	buttonLayout->addWidget(addProgramButton);
	mainLayout->addLayout(buttonLayout);

	mainLayout->addSpacing(10);

	auto checkboxLayout = new QHBoxLayout();
	askToLaunchCheckbox = new QCheckBox("Ask on launch", this);
	autocloseCheckbox = new QCheckBox("Auto-close on OBS exit", this);
	checkboxLayout->addStretch();
	checkboxLayout->addWidget(enableCheckbox);
	checkboxLayout->addSpacing(30);
	checkboxLayout->addWidget(askToLaunchCheckbox);
	checkboxLayout->addSpacing(30);
	checkboxLayout->addWidget(autocloseCheckbox);
	checkboxLayout->addStretch();
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
	connect(addAppButton, &QPushButton::clicked, this, &SettingsWidget::onAddStartMenuProgram);
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
	savedSnapshot = snapshot();

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

bool SettingsWidget::saveSettings()
{
	draft.enabled = enableCheckbox->isChecked();
	draft.askToLaunch = askToLaunchCheckbox->isChecked();
	draft.autoclose = autocloseCheckbox->isChecked();
	draft.currentLoadout = loadoutCombo->currentText().toStdString();
	if (!draft.Save()) {
		QMessageBox::warning(this, "Autostarter", "The settings could not be saved.");
		return false;
	}

	auto &config = PluginConfig::Get();
	config.enabled = draft.enabled;
	config.askToLaunch = draft.askToLaunch;
	config.autoclose = draft.autoclose;
	config.currentLoadout = draft.currentLoadout;
	config.loadouts = draft.loadouts;
	savedSnapshot = snapshot();
	return true;
}

void SettingsWidget::onSave()
{
	if (saveSettings())
		close();
}

QString SettingsWidget::snapshot() const
{
	QStringList parts;
	parts << (enableCheckbox->isChecked() ? "1" : "0") << (askToLaunchCheckbox->isChecked() ? "1" : "0")
	      << (autocloseCheckbox->isChecked() ? "1" : "0") << loadoutCombo->currentText();
	for (const auto &loadout : draft.loadouts) {
		parts << QString::fromStdString("L:" + loadout.name);
		for (const auto &program : loadout.programs) {
			parts << QStringList{QString::fromStdString(program.path),
					     QString::fromStdString(program.executable),
					     QString::fromStdString(program.arguments),
					     program.minimized ? "1" : "0", program.autoclose ? "1" : "0",
					     QString::number(program.delaySeconds)}
					 .join(QChar(0x1f));
		}
	}
	return parts.join(QChar(0x1e));
}

void SettingsWidget::closeEvent(QCloseEvent *event)
{
	if (snapshot() != savedSnapshot) {
		QMessageBox box(this);
		box.setWindowTitle("Autostarter");
		box.setText("You have unsaved changes.");
		box.setInformativeText("Do you want to save them before closing?");
		box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
		box.setDefaultButton(QMessageBox::Save);
		const auto choice = box.exec();
		if (choice == QMessageBox::Cancel || (choice == QMessageBox::Save && !saveSettings())) {
			event->ignore();
			return;
		}
	}
	QWidget::closeEvent(event);
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
	// The app list is scanned the first time Add App is clicked, then reused (and refreshed now and then).
	using namespace std::chrono;
	if (!appScan.valid() || steady_clock::now() - appScanStarted > minutes(10))
		StartAppScan();

	QDialog dialog(this);
	dialog.setWindowTitle("Add an app");
	auto *layout = new QVBoxLayout(&dialog);
	auto *search = new QLineEdit(&dialog);
	search->setPlaceholderText("Search apps");
	auto *status = new QLabel("Loading apps… this only takes a few seconds the first time.", &dialog);
	auto *list = new QListWidget(&dialog);
	auto *showAll = new QCheckBox("Also show uninstallers, help files and web links", &dialog);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(search);
	layout->addWidget(status);
	layout->addWidget(list);
	layout->addWidget(showAll);
	layout->addWidget(buttons);
	status->hide();

	// Icons are slow to fetch for hundreds of apps, so fill them in a few at a time after the dialog opens.
	list->setIconSize(QSize(20, 20));
	auto *iconTimer = new QTimer(&dialog);
	int nextIcon = 0;
	connect(iconTimer, &QTimer::timeout, &dialog, [list, iconTimer, &nextIcon] {
		for (int n = 0; n < 6 && nextIcon < list->count(); ++n, ++nextIcon) {
			auto *item = list->item(nextIcon);
			const auto id = item->data(Qt::UserRole + 1).toString();
			item->setIcon(id.isEmpty() ? FileIcon(item->data(Qt::UserRole).toString())
						   : LoadShellApp(id).icon);
		}
		if (nextIcon >= list->count())
			iconTimer->stop();
	});

	auto *add = buttons->button(QDialogButtonBox::Ok);
	add->setEnabled(false);
	const auto applyFilter = [list, add, search, showAll] {
		const auto query = search->text();
		for (int i = 0; i < list->count(); ++i) {
			auto *item = list->item(i);
			const bool hiddenAsNotAnApp = !showAll->isChecked() && item->data(Qt::UserRole + 2).toBool();
			item->setHidden(hiddenAsNotAnApp || !item->text().contains(query, Qt::CaseInsensitive));
		}
		auto *current = list->currentItem();
		add->setEnabled(current && !current->isHidden());
	};
	const auto populate = [&] {
		for (const auto &candidate : appScan.get()) {
			auto *item = new QListWidgetItem(candidate.name, list);
			item->setData(Qt::UserRole, candidate.file);
			item->setData(Qt::UserRole + 1, candidate.id);
			item->setData(Qt::UserRole + 2, candidate.notAnApp);
			item->setToolTip(candidate.file.isEmpty() ? "Microsoft Store app" : candidate.file);
		}
		status->hide();
		search->setEnabled(true);
		list->setEnabled(true);
		showAll->setEnabled(true);
		search->setFocus();
		nextIcon = 0;
		iconTimer->start(15);
		applyFilter();
	};
	if (appScan.wait_for(seconds(0)) == std::future_status::ready) {
		populate();
	} else {
		// Still scanning: show the dialog straight away and fill it in as soon as the scan finishes.
		status->show();
		search->setEnabled(false);
		list->setEnabled(false);
		showAll->setEnabled(false);
		auto *waitTimer = new QTimer(&dialog);
		connect(waitTimer, &QTimer::timeout, &dialog, [&, waitTimer] {
			if (appScan.wait_for(seconds(0)) == std::future_status::ready) {
				waitTimer->stop();
				populate();
			}
		});
		waitTimer->start(50);
	}
	connect(list, &QListWidget::currentItemChanged, &dialog,
		[add](QListWidgetItem *item) { add->setEnabled(item && !item->isHidden()); });
	connect(search, &QLineEdit::textChanged, &dialog, applyFilter);
	connect(showAll, &QCheckBox::toggled, &dialog, applyFilter);
	connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.resize(520, 460);
	if (dialog.exec() == QDialog::Accepted && list->currentItem()) {
		const auto id = list->currentItem()->data(Qt::UserRole + 1).toString();
		if (!id.isEmpty())
			addShellApp(id);
		else
			addProgram(list->currentItem()->data(Qt::UserRole).toString());
	}
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

void SettingsWidget::addShellApp(const QString &id)
{
	if (auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString())) {
		Program program;
		program.path = kShellAppsPath.toStdString();
		program.executable = id.toStdString();
		loadout->programs.push_back(std::move(program));
		UpdateProgramList();
		programsList->setCurrentRow(programsList->count() - 1);
	}
}

void SettingsWidget::removeProgram(int row)
{
	auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString());
	if (loadout && row >= 0 && row < static_cast<int>(loadout->programs.size())) {
		loadout->programs.erase(loadout->programs.begin() + row);
		UpdateProgramList();
		programsList->setCurrentRow(qMin(row, programsList->count() - 1));
	}
}

void SettingsWidget::moveProgram(int row, int offset)
{
	const int destination = row + offset;
	auto *loadout = draft.GetLoadout(loadoutCombo->currentText().toStdString());
	if (loadout && row >= 0 && destination >= 0 && destination < static_cast<int>(loadout->programs.size())) {
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
		const int count = static_cast<int>(loadout->programs.size());
		for (int index = 0; index < count; ++index) {
			const auto &program = loadout->programs[index];
			QString fullPath = QString::fromStdString(
				program.path + "/" + program.executable);
			auto item = new QListWidgetItem(programsList);
			auto widget = new ProgramListItem(
				fullPath, program, programsList);
			// Size the row from its contents so the theme's control padding never clips anything.
			item->setSizeHint(QSize(0, qMax(widget->sizeHint().height(), widget->minimumSizeHint().height()) + 6));
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

			// These rebuild the list, which destroys this row, so defer them.
			widget->upButton->setEnabled(index > 0);
			widget->downButton->setEnabled(index < count - 1);
			connect(widget->upButton, &QToolButton::clicked, this,
				[this, index] { QTimer::singleShot(0, this, [this, index] { moveProgram(index, -1); }); });
			connect(widget->downButton, &QToolButton::clicked, this,
				[this, index] { QTimer::singleShot(0, this, [this, index] { moveProgram(index, 1); }); });
			connect(widget->removeButton, &QToolButton::clicked, this,
				[this, index] { QTimer::singleShot(0, this, [this, index] { removeProgram(index); }); });
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
