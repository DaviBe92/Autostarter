#include "autostart.hpp"
#include "config.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <obs-module.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

namespace {
struct CloseWindowsHandle {
	void operator()(HANDLE handle) const { CloseHandle(handle); }
};

using Handle = std::unique_ptr<void, CloseWindowsHandle>;

struct LaunchedProgram {
	Handle process;
	Handle job;
	std::string name;
	std::wstring path;
	bool autoclose;
};

std::vector<LaunchedProgram> launchedPrograms;
std::unique_ptr<QObject> launchContext;
bool launchPending = false;

std::wstring NativePath(const QString &path)
{
	return QDir::toNativeSeparators(QDir::cleanPath(path)).toStdWString();
}

std::vector<PROCESSENTRY32W> SnapshotProcesses()
{
	std::vector<PROCESSENTRY32W> entries;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return entries;

	Handle snapshotHandle(snapshot);
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (!Process32FirstW(snapshot, &entry))
		return entries;

	do {
		entries.push_back(entry);
	} while (Process32NextW(snapshot, &entry));
	return entries;
}

bool IsProcessRunning(const std::vector<PROCESSENTRY32W> &entries, const std::wstring &path,
		      const std::wstring &name)
{
	for (const auto &entry : entries) {
		if (CompareStringOrdinal(name.c_str(), -1, entry.szExeFile, -1, TRUE) != CSTR_EQUAL)
			continue;

		Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
		if (!process)
			continue;

		std::wstring runningPath(32768, L'\0');
		DWORD length = static_cast<DWORD>(runningPath.size());
		if (QueryFullProcessImageNameW(process.get(), 0, runningPath.data(), &length) &&
		    CompareStringOrdinal(path.c_str(), -1, runningPath.data(), static_cast<int>(length), TRUE) ==
			    CSTR_EQUAL)
			return true;
	}
	return false;
}

bool IsActive(const LaunchedProgram &program)
{
	if (program.job) {
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
		if (QueryInformationJobObject(program.job.get(), JobObjectBasicAccountingInformation,
					      &info, sizeof(info), nullptr))
			return info.ActiveProcesses > 0;
		return true;
	}
	return WaitForSingleObject(program.process.get(), 0) != WAIT_OBJECT_0;
}

struct ComApartment {
	HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	~ComApartment()
	{
		if (SUCCEEDED(result))
			CoUninitialize();
	}
};

bool ReadExecutableShortcut(std::wstring &path, std::wstring &directory, std::wstring &arguments)
{
	ComApartment apartment;
	if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
		return false;

	using Microsoft::WRL::ComPtr;
	ComPtr<IShellLinkW> link;
	ComPtr<IPersistFile> file;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
	    FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ)))
		return false;

	ComPtr<IShellLinkDataList> data;
	DWORD flags = 0;
	if (SUCCEEDED(link.As(&data)) && SUCCEEDED(data->GetFlags(&flags)) && (flags & SLDF_RUNAS_USER))
		return false;

	wchar_t buffer[32768]{};
	if (FAILED(link->GetPath(buffer, static_cast<int>(std::size(buffer)), nullptr, 0)))
		return false;
	const QFileInfo target(QString::fromWCharArray(buffer));
	if (!target.isAbsolute() || !target.isFile() ||
	    target.suffix().compare("exe", Qt::CaseInsensitive) != 0)
		return false;

	path = NativePath(target.absoluteFilePath());
	buffer[0] = L'\0';
	link->GetWorkingDirectory(buffer, static_cast<int>(std::size(buffer)));
	directory = *buffer ? NativePath(QString::fromWCharArray(buffer)) : NativePath(target.absolutePath());
	buffer[0] = L'\0';
	if (SUCCEEDED(link->GetArguments(buffer, static_cast<int>(std::size(buffer)))) && *buffer)
		arguments = std::wstring(buffer) + (arguments.empty() ? L"" : L" " + arguments);
	return true;
}

bool LaunchProgram(const Program &program, const std::vector<PROCESSENTRY32W> &entries)
{
	if (program.path.empty() || program.executable.empty())
		return false;

	auto directory = NativePath(QString::fromStdString(program.path));
	const auto fileName = QString::fromStdString(program.executable);
	auto path = NativePath(QDir(QString::fromStdString(program.path)).absoluteFilePath(fileName));
	auto arguments = QString::fromStdString(program.arguments).toStdWString();
	const int show = program.minimized ? SW_SHOWMINIMIZED : SW_SHOWNORMAL;

	const bool executable = !fileName.contains('.') || fileName.endsWith(".exe", Qt::CaseInsensitive);
	const bool executableShortcut = fileName.endsWith(".lnk", Qt::CaseInsensitive) &&
					ReadExecutableShortcut(path, directory, arguments);
	if (!executable && !executableShortcut) {
		ComApartment apartment;
		const auto result = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", path.c_str(), arguments.empty() ? nullptr : arguments.c_str(),
				      directory.c_str(), show));
		if (result > 32)
			return true;
		blog(LOG_WARNING, "[Autostarter] Could not open '%s' (shell error %ld)",
		     program.executable.c_str(), static_cast<long>(result));
		return false;
	}

	const bool tracked = std::any_of(launchedPrograms.begin(), launchedPrograms.end(),
					[&path](const LaunchedProgram &entry) {
						return CompareStringOrdinal(path.c_str(), -1, entry.path.c_str(),
									    -1, TRUE) == CSTR_EQUAL;
					});
	if (tracked || IsProcessRunning(entries, path, QFileInfo(QString::fromStdWString(path)).fileName().toStdWString())) {
		blog(LOG_INFO, "[Autostarter] '%s' is already running", program.executable.c_str());
		return true;
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	if (program.minimized) {
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_SHOWMINIMIZED;
	}

	// Assign the suspended launcher before it can spawn any children.
	Handle job(CreateJobObjectW(nullptr, nullptr));
	std::wstring commandLine = L"\"" + path + L"\"";
	if (!arguments.empty())
		commandLine += L" " + arguments;
	PROCESS_INFORMATION info{};
	if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
			    nullptr, directory.c_str(), &startup, &info)) {
		blog(LOG_WARNING, "[Autostarter] Could not launch '%s' (error %lu)",
		     program.executable.c_str(), GetLastError());
		return false;
	}

	Handle process(info.hProcess);
	Handle thread(info.hThread);
	if (!job || !AssignProcessToJobObject(job.get(), process.get())) {
		blog(LOG_WARNING, "[Autostarter] Child processes of '%s' cannot be tracked (error %lu)",
		     program.executable.c_str(), GetLastError());
		job.reset();
	}

	launchedPrograms.push_back({std::move(process), std::move(job), program.executable, path, program.autoclose});
	if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
		blog(LOG_WARNING, "[Autostarter] Could not resume '%s' (error %lu)",
		     program.executable.c_str(), GetLastError());
		TerminateProcess(info.hProcess, 1);
		launchedPrograms.pop_back();
		return false;
	}
	return true;
}

bool LaunchBatch(const Loadout &loadout, size_t first, bool delayElapsed)
{
	QElapsedTimer timer;
	timer.start();
	std::erase_if(launchedPrograms, [](const LaunchedProgram &program) { return !IsActive(program); });
	std::vector<PROCESSENTRY32W> entries;
	bool snapped = false;
	bool success = true;
	size_t next = first;
	for (; next < loadout.programs.size(); ++next) {
		const auto &program = loadout.programs[next];
		if (program.delaySeconds > 0 && !(next == first && delayElapsed)) {
			if (!launchContext)
				launchContext = std::make_unique<QObject>();
			launchPending = true;
			QTimer::singleShot(program.delaySeconds * 1000, Qt::PreciseTimer, launchContext.get(),
					   [loadout, next] {
						   launchPending = false;
						   LaunchBatch(loadout, next, true);
					   });
			break;
		}

		if (!snapped) {
			entries = SnapshotProcesses();
			snapped = true;
		}
		if (!LaunchProgram(program, entries))
			success = false;
	}

	if (next != first)
		blog(LOG_INFO, "[Autostarter] Processed %zu entries in loadout '%s' in %lld ms",
		     next - first, loadout.name.c_str(), static_cast<long long>(timer.elapsed()));
	return success;
}
} // namespace

bool AutoStarter::LaunchPrograms(const std::string &loadoutName)
{
	auto &config = PluginConfig::Get();
	const auto &name = loadoutName.empty() ? config.currentLoadout : loadoutName;
	const auto *loadout = config.GetLoadout(name);
	if (!loadout) {
		blog(LOG_WARNING, "[Autostarter] Loadout '%s' not found", name.c_str());
		return false;
	}

	return LaunchPrograms(*loadout);
}

bool AutoStarter::LaunchPrograms(const Loadout &loadout)
{
	if (launchPending) {
		blog(LOG_WARNING, "[Autostarter] A launch sequence is already in progress");
		return false;
	}
	return LaunchBatch(loadout, 0, false);
}

bool AutoStarter::QuitPrograms(bool automatic)
{
	launchContext.reset();
	launchPending = false;
	bool success = true;
	std::erase_if(launchedPrograms, [automatic, &success](const LaunchedProgram &program) {
		if ((automatic && (!PluginConfig::Get().autoclose || !program.autoclose)) || !IsActive(program))
			return true;

		const BOOL terminated = program.job ? TerminateJobObject(program.job.get(), 0)
						    : TerminateProcess(program.process.get(), 0);
		if (terminated)
			return true;

		const DWORD error = GetLastError();
		if (!IsActive(program))
			return true;
		blog(LOG_WARNING, "[Autostarter] Could not quit '%s' (error %lu)", program.name.c_str(), error);
		success = false;
		return automatic;
	});
	return success;
}
