#include "PCH.h"

#include "DevBenchTool.h"

#include "DevBench/DevBenchAPI.h"
#include "Settings.h"
#include "Swim.h"
#include "utils/Logger.h"

#include <format>
#include <string>
#include <string_view>

// DevBench tool "ssc.control" (rules 31 and 64). A real test still presses the real button; hold and dive prove the
// movement path and the pose, not the input path.
namespace DevBenchTool
{
	namespace
	{
		// The value of a top-level JSON member, as text: a quoted string or a bare word. Empty when absent.
		std::string Get(std::string_view a_json, const char* a_name)
		{
			const std::string key = std::format("\"{}\"", a_name);
			auto pos = a_json.find(key);
			if (pos == std::string_view::npos) { return {}; }
			pos = a_json.find(':', pos + key.size());
			if (pos == std::string_view::npos) { return {}; }
			++pos;
			while (pos < a_json.size() && (a_json[pos] == ' ' || a_json[pos] == '\t')) { ++pos; }
			if (pos >= a_json.size()) { return {}; }
			if (a_json[pos] == '"')
			{
				const auto close = a_json.find('"', pos + 1);
				return close == std::string_view::npos ? std::string{} : std::string(a_json.substr(pos + 1, close - pos - 1));
			}
			std::string out;
			while (pos < a_json.size() && a_json[pos] != ',' && a_json[pos] != '}' && a_json[pos] != ' ') { out += a_json[pos++]; }
			return out;
		}

		int RowIndex(const std::string& a_name)
		{
			for (int r = 0; r < settings::kRowCount; ++r)
			{
				if (a_name == settings::RowName(r)) { return r; }
			}
			return -1;
		}

		bool SetByName(const std::string& a_name, const std::string& a_value)
		{
			try
			{
				if (a_name == "bEnabled") { settings::enabled = a_value == "1"; }
				else if (a_name == "bSwimVertical") { settings::swimVertical = a_value == "1"; }
				else if (a_name == "fSwimVerticalSpeed") { settings::swimVerticalSpeed = std::stof(a_value); }
				else if (a_name == "bDive") { settings::dive = a_value == "1"; }
				else if (a_name == "bDiveAnimation") { settings::diveAnimation = a_value == "1"; }
				else if (a_name == "fDiveSinkSpeed") { settings::diveSinkSpeed = std::stof(a_value); }
				else if (a_name == "fDiveSinkSeconds") { settings::diveSinkSeconds = std::stof(a_value); }
				else { return false; }
			}
			catch (...)
			{
				return false;
			}
			return true;
		}

		void ControlTool(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const std::string_view args = a_args ? a_args : "";
			const std::string op = Get(args, "op");
			std::string extra = "true";
			if (op == "hold")
			{
				const int row = RowIndex(Get(args, "row"));
				if (row < 0) { extra = "false,\"error\":\"row is Ascend, Descend or Dive\""; }
				else
				{
					const std::string ms = Get(args, "ms");
					swim::DriveHold(row, ms.empty() ? 1000 : std::atoi(ms.c_str()));
				}
			}
			else if (op == "dive") { swim::DriveDive(); }
			else if (op == "set")
			{
				if (!SetByName(Get(args, "name"), Get(args, "value"))) { extra = "false,\"error\":\"unknown name or bad value\""; }
				else { settings::Save(); }
			}
			else if (op == "restore")
			{
				settings::RestoreDefaults();
				settings::Save();
			}
			else if (op == "reload") { settings::Load(); }
			const std::string out = std::format(R"({{"ok":{},"op":"{}","ini":"{}",{}}})", extra, op.empty() ? "state" : op, "SeparateSwimmingControls.ini", swim::StateJson());
			a_write(a_sink, out.c_str());
		}
	}

	void Init(bool a_lastAttempt)
	{
		static bool registered = false;
		if (registered) { return; }
		DevBenchAPI::IDevBenchInterface001* devBench = DevBenchAPI::GetDevBenchInterface001();
		if (!devBench)
		{
			if (a_lastAttempt) { logger::info("DevBench not detected; skipping the \"ssc.control\" tool"); }
			return;
		}
		constexpr const char* descriptor =
			"{"
			"\"description\":\"Separate Swimming Controls live state and driver. op=state (default): settings, swimming state, keys and whether each is held, counters, dive state and what Simple Diving System has answered. "
			"op=hold row=Ascend|Descend ms=N: as if the row's key were held. op=dive: what the Dive key queues. op=set name=<INI key> value=<v>: sets and saves. op=restore: defaults, saved. op=reload: re-reads the INI.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"row\":{\"type\":\"string\"},\"ms\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}}},"
			"\"readOnly\":false"
			"}";
		if (devBench->RegisterTool("ssc.control", descriptor, &ControlTool, nullptr))
		{
			logger::info("registered \"ssc.control\" with DevBench (build {})", devBench->GetBuildNumber());
			registered = true;
		}
	}
}
