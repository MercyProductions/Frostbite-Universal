These are local staged Frostbite-Universal tool files.

Compiled binaries are ignored by Git. Rebuild them locally or attach them to a GitHub Release.

SDK dump injection target:

Tools\INJECT_THIS_FOR_SDK_DUMP_FrostbiteSDKGenerator.dll

Inject that when you want the SDK generator DLL to auto-run inside an owned/local process.

It opens or attaches a console, captures only the app it was loaded into, and writes output to:

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
- FrostbiteSDKGenerator.dll: callable SDK generator library
- INJECT_THIS_FOR_SDK_DUMP_FrostbiteSDKGenerator.dll: injectable SDK dumper build
- FrostbiteSDKGenerator.h: C API header for the generator DLL
- FrostbiteUniversal.h: C API/types for the runtime Universal DLL and owned-project bridge
