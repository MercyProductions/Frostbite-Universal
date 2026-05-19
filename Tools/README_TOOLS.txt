These are local staged Frostbite-Universal tool files.

Compiled binaries are ignored by Git. Rebuild them locally or attach them to a GitHub Release.

SDK dump target:

INJECT_THIS_FrostbiteUniversal.dll

Inject that when you want the Universal ImGui tool inside an owned/local process, then open the SDK tab and run the full SDK dump.

It opens or attaches a console for the embedded generator backend, captures only the app it was loaded into, and writes output to:

GeneratedSDK\Injected_<process>_<pid>

The generated SDK folder includes:

GeneratedSDK\Injected_<process>_<pid>\SDK\SDK.hpp

Include that from another C++17 project to reuse the dumped SDK metadata and research reports.

For your owned game bridge, see:

Tools\PROJECT_BRIDGE_EXAMPLE.md

Runtime universal diagnostics target:

INJECT_THIS_FrostbiteUniversal.dll

Tool files:

- FrostbiteSDKGenerator.exe: command-line SDK/report generator
- FrostbiteSDKGenerator.h: C API header for the standalone and embedded generator backend
- FrostbiteUniversal.h: C API/types for the runtime Universal DLL, embedded dumper backend, and owned-project bridge
