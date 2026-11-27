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
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <map>
#include <set>

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
std::unique_ptr<QObject> minimizeContext;
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

// Many apps ignore the "start minimized" request, so their windows are minimized after they appear.
struct MinimizeWatch {
	Handle process;
	Handle job; // optional, covers child processes
	DWORD processId = 0;
	std::map<HWND, int> seen; // window -> tick when it was first seen
	int ticks = 0;
};

struct ChildSearch {
	DWORD processId;
	bool found = false;
};

BOOL CALLBACK FindChildOfProcess(HWND child, LPARAM parameter)
{
	auto &search = *reinterpret_cast<ChildSearch *>(parameter);
	DWORD childProcess = 0;
	GetWindowThreadProcessId(child, &childProcess);
	if (childProcess == search.processId) {
		search.found = true;
		return FALSE;
	}
	return TRUE;
}

BOOL CALLBACK MinimizeAppWindow(HWND window, LPARAM parameter)
{
	auto &watch = *reinterpret_cast<MinimizeWatch *>(parameter);
	if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
	    (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
		return TRUE;

	DWORD windowProcess = 0;
	GetWindowThreadProcessId(window, &windowProcess);
	bool belongs = windowProcess == watch.processId;
	if (!belongs) {
		// Windows draws the frame of Store (UWP) apps in a host process, with the app's own window inside it.
		wchar_t className[64]{};
		if (GetClassNameW(window, className, static_cast<int>(std::size(className))) &&
		    wcscmp(className, L"ApplicationFrameWindow") == 0) {
			ChildSearch search{watch.processId};
			EnumChildWindows(window, FindChildOfProcess, reinterpret_cast<LPARAM>(&search));
			belongs = search.found;
		}
	}
	if (!belongs && watch.job) {
		Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, windowProcess));
		BOOL inJob = FALSE;
		belongs = owner && IsProcessInJob(owner.get(), watch.job.get(), &inJob) && inJob;
	}
	if (!belongs)
		return TRUE;

	// Some apps restore their own window right after starting, so keep minimizing it for a few
	// seconds. After that the window is left alone so the user can restore it.
	const auto entry = watch.seen.try_emplace(window, watch.ticks);
	if (IsIconic(window) || watch.ticks - entry.first->second > 15)
		return TRUE;

	ShowWindow(window, SW_SHOWMINNOACTIVE);
	blog(LOG_INFO, "[Autostarter] Minimized a window of process %lu (tick %d)", watch.processId, watch.ticks);
	return TRUE;
}

void MinimizeWhenShown(Handle process, Handle job, DWORD processId)
{
	if (!minimizeContext)
		minimizeContext = std::make_unique<QObject>();

	auto watch = std::make_shared<MinimizeWatch>();
	watch->process = std::move(process);
	watch->job = std::move(job);
	watch->processId = processId;

	auto *timer = new QTimer(minimizeContext.get());
	QObject::connect(timer, &QTimer::timeout, timer, [watch, timer] {
		EnumWindows(MinimizeAppWindow, reinterpret_cast<LPARAM>(watch.get()));
		// Stop after 20 seconds, or once the app is gone.
		const bool gone = watch->process && WaitForSingleObject(watch->process.get(), 0) == WAIT_OBJECT_0 &&
				  (!watch->job || [&] {
					  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
					  return QueryInformationJobObject(watch->job.get(),
									   JobObjectBasicAccountingInformation, &info,
									   sizeof(info), nullptr) &&
						 info.ActiveProcesses == 0;
				  }());
		if (++watch->ticks >= 100 || gone) {
			timer->stop();
			timer->deleteLater();
		}
	});
	timer->start(200);
	blog(LOG_INFO, "[Autostarter] Watching process %lu to start it minimized", processId);
}

Handle DuplicateForWatch(HANDLE handle)
{
	HANDLE copy = nullptr;
	if (handle && DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, FALSE,
				      DUPLICATE_SAME_ACCESS))
		return Handle(copy);
	return Handle();
}

bool LaunchProgram(const Program &program, const std::vector<PROCESSENTRY32W> &entries)
{
	if (program.path.empty() || program.executable.empty())
		return false;

	// Microsoft Store apps are started through the shell by their Apps folder id.
	if (program.path == "shell:AppsFolder") {
		const auto appId = QString::fromStdString(program.executable).toStdWString();
		ComApartment apartment;

		// Activating the app through the activation manager returns its process id, which lets the
		// plugin close the app on exit and find its window to minimize it.
		using Microsoft::WRL::ComPtr;
		ComPtr<IApplicationActivationManager> manager;
		DWORD processId = 0;
		HRESULT activated = CoCreateInstance(__uuidof(ApplicationActivationManager), nullptr,
						     CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
		if (SUCCEEDED(activated)) {
			CoAllowSetForegroundWindow(manager.Get(), nullptr);
			activated = manager->ActivateApplication(appId.c_str(), nullptr, AO_NOERRORUI, &processId);
		}

		if (FAILED(activated) || processId == 0) {
			// Fall back to a plain shell launch, which can't be tracked.
			const auto target = L"shell:AppsFolder\\" + appId;
			const auto result = reinterpret_cast<INT_PTR>(
				ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
					      program.minimized ? SW_SHOWMINIMIZED : SW_SHOWNORMAL));
			if (result > 32)
				return true;
			blog(LOG_WARNING, "[Autostarter] Could not start Store app '%s' (error 0x%08lX)",
			     program.executable.c_str(), static_cast<unsigned long>(activated));
			return false;
		}

		Handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
					   processId));
		if (!process)
			return true; // Started, but it can't be tracked.

		// Activating a running app returns its existing process. Leave those alone, like other apps
		// that were already open before OBS started.
		FILETIME created{}, exited{}, kernel{}, user{}, now{};
		GetSystemTimeAsFileTime(&now);
		if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
			ULARGE_INTEGER start{}, current{};
			start.LowPart = created.dwLowDateTime;
			start.HighPart = created.dwHighDateTime;
			current.LowPart = now.dwLowDateTime;
			current.HighPart = now.dwHighDateTime;
			if (current.QuadPart > start.QuadPart && current.QuadPart - start.QuadPart > 5ULL * 10000000ULL) {
				blog(LOG_INFO, "[Autostarter] '%s' is already running", program.executable.c_str());
				return true;
			}
		}

		launchedPrograms.push_back({std::move(process), Handle(), program.executable, L"shell:AppsFolder\\" + appId,
					    program.autoclose});
		if (program.minimized)
			MinimizeWhenShown(DuplicateForWatch(launchedPrograms.back().process.get()), Handle(), processId);
		return true;
	}

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
		SHELLEXECUTEINFOW execute{};
		execute.cbSize = sizeof(execute);
		execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
		execute.lpVerb = L"open";
		execute.lpFile = path.c_str();
		execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
		execute.lpDirectory = directory.c_str();
		execute.nShow = show;
		if (ShellExecuteExW(&execute)) {
			Handle process(execute.hProcess);
			if (program.minimized && process)
				MinimizeWhenShown(std::move(process), Handle(), GetProcessId(execute.hProcess));
			return true;
		}
		blog(LOG_WARNING, "[Autostarter] Could not open '%s' (error %lu)", program.executable.c_str(),
		     GetLastError());
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

	if (program.minimized)
		MinimizeWhenShown(DuplicateForWatch(launchedPrograms.back().process.get()),
				  DuplicateForWatch(launchedPrograms.back().job.get()), info.dwProcessId);
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
	minimizeContext.reset();
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
