// Test helper launched by Test-Autostarter.ps1. Records how it was started, then waits to be closed.
//   --name <id>    marker name (writes <out>\<id>.json)
//   --out <dir>    output directory
//   --spawn-child  start a copy of itself so child-process cleanup can be checked
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

static class Marker
{
	static string Json(string value)
	{
		var sb = new StringBuilder("\"");
		foreach (char c in value) {
			if (c == '\\' || c == '"')
				sb.Append('\\').Append(c);
			else if (c < 0x20)
				sb.AppendFormat("\\u{0:x4}", (int)c);
			else
				sb.Append(c);
		}
		return sb.Append('"').ToString();
	}

	static int Main(string[] args)
	{
		string name = null, outDir = null;
		bool spawnChild = false;
		for (int i = 0; i < args.Length; i++) {
			if (args[i] == "--name" && i + 1 < args.Length)
				name = args[++i];
			else if (args[i] == "--out" && i + 1 < args.Length)
				outDir = args[++i];
			else if (args[i] == "--spawn-child")
				spawnChild = true;
		}
		if (name == null || outDir == null)
			return 2;

		string exe = Process.GetCurrentProcess().MainModule.FileName;
		if (spawnChild) {
			var start = new ProcessStartInfo(exe, "--name " + name + "-child --out \"" + outDir + "\"");
			start.UseShellExecute = false;
			Process.Start(start);
		}

		string json = "{" +
			"\"name\":" + Json(name) +
			",\"pid\":" + Process.GetCurrentProcess().Id +
			",\"time\":" + Json(DateTime.UtcNow.ToString("o")) +
			",\"exe\":" + Json(exe) +
			",\"cwd\":" + Json(Environment.CurrentDirectory) +
			",\"commandLine\":" + Json(Environment.CommandLine) +
			",\"args\":[" + string.Join(",", Array.ConvertAll(args, Json)) + "]}";
		File.WriteAllText(Path.Combine(outDir, name + ".json"), json, new UTF8Encoding(false));

		Thread.Sleep(Timeout.Infinite);
		return 0;
	}
}
